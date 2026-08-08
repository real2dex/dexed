// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Headless entry point. No window, no audio device, no message loop: every
// clip is rendered offline on a worker thread and written straight to disk.

#include "../JuceLibraryCode/JuceHeader.h"
#include "PluginProcessor.h"
#include "RenderJob.h"

#include <cstdio>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    const char* usageText = R"(dexed-cli - headless Dexed renderer

USAGE
  dexed-cli render [options] -o OUT.wav
  dexed-cli batch  [options] JOBS.jsonl        (use '-' to read stdin)
  dexed-cli params

RENDER OPTIONS
  -o, --output PATH      output wav (required)
      --syx PATH         cartridge to load
      --program N        program 0-31 within the cartridge
      --note N           midi note 0-127                    (default 60)
      --velocity F       0.0-1.0                            (default 0.8)
      --param NAME=VAL   normalised 0..1 override, repeatable

BATCH OPTIONS
  -j, --threads N        worker threads              (default: hardware cores)
      --report           print one JSON result line per job

SYNTH OPTIONS
      --engine NAME      markI | modern | opl                (default markI)
                         The plugin picked this up from Dexed.xml; the CLI
                         never reads that file, so state it here if you need
                         to match clips rendered by the plugin build.

CLIP OPTIONS (defaults reproduce the original 4.0 s / 48 kHz clip exactly)
      --rate HZ          sample rate                        (default 48000)
      --bits N           16, 24 or 32 (32 = float)          (default 32)
      --channels N       1 or 2 (2 duplicates the mono out) (default 1)
      --bpm F                                               (default 120)
      --sound-beats F    beats held before note off         (default 4)
      --tail-beats F     beats of release captured          (default 4)
      --block-size N     render block size                  (default 512)
      --skip-silent-tail stop the FM engine once the rest of the clip is
                         provably silent; bit-identical output, just faster

  -q, --quiet            suppress the progress summary on stderr

JOB LINES (one JSON object per line)
  {"output": "a.wav", "syx": "rom1a.syx", "program": 3, "note": 60,
   "velocity": 0.8, "params": {"ALGORITHM": 0.5}}
  Only "output" is required; anything omitted falls back to the CLI options.
  Run 'dexed-cli params' for the exact parameter names.
)";

    struct Options
    {
        juce::String command;
        juce::File   jobsFile;
        bool         jobsFromStdin = false;

        juce::File   output;
        juce::File   syx;
        int          program = -1;
        juce::NamedValueSet params;

        RenderSpec   spec;
        int          engineType = DEXED_ENGINE_MARKI;
        int          threads = 0;
        bool         report  = false;
        bool         quiet   = false;
    };

    int parseEngine (const juce::String& name)
    {
        if (name.equalsIgnoreCase ("markI") || name.equalsIgnoreCase ("mark1"))
            return DEXED_ENGINE_MARKI;
        if (name.equalsIgnoreCase ("modern"))
            return DEXED_ENGINE_MODERN;
        if (name.equalsIgnoreCase ("opl"))
            return DEXED_ENGINE_OPL;
        return -1;
    }

    [[noreturn]] void fail (const juce::String& message)
    {
        std::cerr << "dexed-cli: " << message << std::endl;
        std::exit (2);
    }

    juce::String nextArg (const juce::StringArray& args, int& i, const juce::String& flag)
    {
        if (++i >= args.size())
            fail (flag + " needs a value");
        return args[i];
    }

    Options parseArgs (const juce::StringArray& args)
    {
        Options o;

        if (args.isEmpty())
        {
            std::cout << usageText;
            std::exit (0);
        }

        o.command = args[0];

        if (o.command == "-h" || o.command == "--help")
        {
            std::cout << usageText;
            std::exit (0);
        }

        if (o.command != "render" && o.command != "batch" && o.command != "params")
            fail ("unknown command '" + o.command + "' (expected render, batch or params)");

        for (int i = 1; i < args.size(); ++i)
        {
            const juce::String a = args[i];

            if      (a == "-o" || a == "--output")   o.output  = juce::File::getCurrentWorkingDirectory().getChildFile (nextArg (args, i, a));
            else if (a == "--syx")                   o.syx     = juce::File::getCurrentWorkingDirectory().getChildFile (nextArg (args, i, a));
            else if (a == "--program")               o.program = nextArg (args, i, a).getIntValue();
            else if (a == "--note")                  o.spec.midiNote = nextArg (args, i, a).getIntValue();
            else if (a == "--velocity")              o.spec.velocity = (float) nextArg (args, i, a).getDoubleValue();
            else if (a == "--rate")                  o.spec.sampleRate = nextArg (args, i, a).getDoubleValue();
            else if (a == "--bits")                  o.spec.bitDepth = nextArg (args, i, a).getIntValue();
            else if (a == "--channels")              o.spec.channels = nextArg (args, i, a).getIntValue();
            else if (a == "--bpm")                   o.spec.bpm = nextArg (args, i, a).getDoubleValue();
            else if (a == "--sound-beats")           o.spec.soundBeats = nextArg (args, i, a).getDoubleValue();
            else if (a == "--tail-beats")            o.spec.tailBeats = nextArg (args, i, a).getDoubleValue();
            else if (a == "--block-size")            o.spec.blockSize = nextArg (args, i, a).getIntValue();
            else if (a == "--skip-silent-tail")      o.spec.skipDeadVoices = true;
            else if (a == "--engine")
            {
                const juce::String name = nextArg (args, i, a);
                o.engineType = parseEngine (name);
                if (o.engineType < 0)
                    fail ("unknown --engine '" + name + "' (expected markI, modern or opl)");
            }
            else if (a == "--param")
            {
                const juce::String kv = nextArg (args, i, a);
                const int eq = kv.indexOfChar ('=');
                if (eq <= 0)
                    fail ("--param needs NAME=VALUE, got '" + kv + "'");
                o.params.set (juce::Identifier (kv.substring (0, eq)),
                              juce::jlimit (0.0f, 1.0f, (float) kv.substring (eq + 1).getDoubleValue()));
            }
            else if (a == "-j" || a == "--threads")  o.threads = nextArg (args, i, a).getIntValue();
            else if (a == "--report")                o.report = true;
            else if (a == "-q" || a == "--quiet")    o.quiet = true;
            else if (a == "-")                       o.jobsFromStdin = true;
            else if (a.startsWith ("-"))             fail ("unknown option '" + a + "'");
            else                                     o.jobsFile = juce::File::getCurrentWorkingDirectory().getChildFile (a);
        }

        if (o.spec.sampleRate <= 0.0)                fail ("--rate must be positive");
        if (o.spec.bpm <= 0.0)                       fail ("--bpm must be positive");
        if (o.spec.channels != 1 && o.spec.channels != 2)
            fail ("--channels must be 1 or 2");
        if (o.spec.bitDepth != 16 && o.spec.bitDepth != 24 && o.spec.bitDepth != 32)
            fail ("--bits must be 16, 24 or 32");
        if (o.spec.midiNote < 0 || o.spec.midiNote > 127)
            fail ("--note must be 0-127");
        if (o.spec.totalSamples() <= 0)
            fail ("clip length is zero; check --bpm / --sound-beats / --tail-beats");

        if (o.threads <= 0)
            o.threads = (int) juce::jmax (1u, std::thread::hardware_concurrency());

        return o;
    }

    /** Reads job lines from a file or stdin. Blank lines are skipped. */
    juce::StringArray readJobLines (const Options& o)
    {
        juce::StringArray lines;

        if (o.jobsFromStdin || o.jobsFile.getFileName() == "-")
        {
            // Reads to EOF before rendering starts, so a producer must close
            // the pipe (or send all jobs up front) rather than expect a reply
            // per line.
            std::string line;
            while (std::getline (std::cin, line))
                lines.add (juce::String::fromUTF8 (line.c_str()));
        }
        else if (o.jobsFile == juce::File())
        {
            fail ("batch needs a jobs file (or '-' for stdin)");
        }
        else if (! o.jobsFile.existsAsFile())
        {
            fail ("no such jobs file: " + o.jobsFile.getFullPathName());
        }
        else
        {
            lines.addLines (o.jobsFile.loadFileAsString());
        }

        lines.removeEmptyStrings();
        return lines;
    }

    int runParams()
    {
        for (const auto& name : BatchRenderer::parameterNames())
            std::cout << name << "\n";
        return 0;
    }

    int runRender (const Options& o)
    {
        if (o.output == juce::File())
            fail ("render needs -o/--output");

        RenderJob job;
        job.output  = o.output;
        job.syx     = o.syx;
        job.program = o.program;
        job.params  = o.params;
        job.spec    = o.spec;

        DexedAudioProcessor::initSharedTables (o.spec.sampleRate);

        DexedAudioProcessor proc;
        proc.setEngineType (o.engineType);
        proc.prepareForOfflineRender (o.spec.sampleRate, o.spec.blockSize);

        const RenderResult r = BatchRenderer::renderOne (proc, job, 0);

        if (! r.ok)
        {
            std::cerr << "dexed-cli: " << r.error << std::endl;
            return 1;
        }

        if (! o.quiet)
            std::cerr << "wrote " << o.output.getFullPathName() << std::endl;

        return 0;
    }

    int runBatch (const Options& o)
    {
        const juce::StringArray lines = readJobLines (o);

        juce::Array<RenderJob> jobs;
        int parseErrors = 0;

        for (int i = 0; i < lines.size(); ++i)
        {
            const juce::var json = juce::JSON::parse (lines[i]);

            RenderJob job;
            const juce::String error = json.isVoid() ? juce::String ("invalid JSON")
                                                     : RenderJob::fromJson (json, o.spec, job);
            if (error.isNotEmpty())
            {
                std::cerr << "dexed-cli: line " << (i + 1) << ": " << error << std::endl;
                ++parseErrors;
                continue;
            }

            if (job.syx == juce::File())
                job.syx = o.syx;
            if (job.program < 0)
                job.program = o.program;

            // CLI --param values act as the baseline; the job line wins on conflict.
            for (int p = 0; p < o.params.size(); ++p)
                if (! job.params.contains (o.params.getName (p)))
                    job.params.set (o.params.getName (p), o.params.getValueAt (p));

            jobs.add (job);
        }

        if (parseErrors > 0)
            return 2;

        if (jobs.isEmpty())
        {
            std::cerr << "dexed-cli: no jobs to render" << std::endl;
            return 0;
        }

        const double startMs = juce::Time::getMillisecondCounterHiRes();

        std::atomic<int> succeeded { 0 };
        std::atomic<int> failed    { 0 };

        BatchRenderer renderer (o.threads, o.spec.sampleRate, o.spec.blockSize, o.engineType);

        renderer.run (jobs, [&] (const RenderResult& r)
        {
            if (r.ok)
                ++succeeded;
            else
            {
                ++failed;
                std::cerr << "dexed-cli: job " << r.index << ": " << r.error << std::endl;
            }

            if (o.report)
            {
                juce::DynamicObject::Ptr line = new juce::DynamicObject();
                line->setProperty ("index", r.index);
                line->setProperty ("ok", r.ok);
                line->setProperty ("output", jobs.getReference (r.index).output.getFullPathName());
                if (! r.ok)
                    line->setProperty ("error", r.error);

                std::cout << juce::JSON::toString (juce::var (line.get()), true) << "\n";
                std::cout.flush();
            }
        });

        const double elapsedSec = (juce::Time::getMillisecondCounterHiRes() - startMs) / 1000.0;

        if (! o.quiet)
        {
            std::cerr << "rendered " << succeeded.load() << "/" << jobs.size()
                      << " clips in " << juce::String (elapsedSec, 2) << " s"
                      << " (" << juce::String (jobs.size() / juce::jmax (1.0e-9, elapsedSec), 1)
                      << " clips/s, " << o.threads << " threads)" << std::endl;
        }

        return failed.load() > 0 ? 1 : 0;
    }
}

int main (int argc, char* argv[])
{
    // Brings up the JUCE runtime (and a MessageManager we never pump). No
    // display is touched: nothing here constructs a Component.
    juce::ScopedJuceInitialiser_GUI juceInit;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i)
        args.add (juce::String::fromUTF8 (argv[i]));

    const Options o = parseArgs (args);

    if (o.command == "params") return runParams();

    if (o.params.size() > 0)
    {
        const juce::StringArray known = BatchRenderer::parameterNames();
        for (int i = 0; i < o.params.size(); ++i)
            if (! known.contains (o.params.getName (i).toString()))
                fail ("unknown --param '" + o.params.getName (i).toString()
                      + "' (run 'dexed-cli params' for the list)");
    }

    if (o.command == "render") return runRender (o);
    return runBatch (o);
}
