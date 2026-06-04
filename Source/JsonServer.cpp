// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#include "JsonServer.h"
#include "PluginProcessor.h"

JsonServer::JsonServer(DexedAudioProcessor& p)
    : juce::Thread("JsonServer"), processor(p)
{
    for (int i = 0; i < processor.getNumParameters(); ++i)
        paramToIndex[processor.getParameterName(i).toStdString()] = i;
}

JsonServer::~JsonServer()
{
    signalThreadShouldExit();
    server.close();
    stopThread(2000);
}

void JsonServer::run()
{
    // Try ports sequentially so multiple VST instances each get a unique port.
    int bound = -1;
    for (int tryPort = DEFAULT_PORT; tryPort < DEFAULT_PORT + MAX_PORT_TRIES; ++tryPort)
    {
        if (server.createListener(tryPort))
        {
            bound = tryPort;
            break;
        }
    }

    if (bound < 0)
        return;

    port   = bound;
    isListening = true;

    while (!threadShouldExit())
    {
        juce::StreamingSocket* client = server.waitForNextConnection();
        if (client == nullptr)
            break;

        ++clients;
        handleClient(*client);
        delete client;
        --clients;
    }

    isListening = false;
}

void JsonServer::applyJson(const juce::var& json)
{
    juce::DynamicObject* obj = json.getDynamicObject();
    if (obj == nullptr)
        return;

    int applied = 0;
    for (auto& prop : obj->getProperties())
    {
        auto it = paramToIndex.find(prop.name.toString().toStdString());
        if (it == paramToIndex.end())
        {
            continue;
        }
        float value = juce::jlimit(0.0f, 1.0f, (float)prop.value);
        processor.setParameter(it->second, value);
        ++applied;
    }

    if (applied > 0)
    {
        processor.forceRefreshUI = true;
        processor.triggerAsyncUpdate();
    }
}

void JsonServer::sendJson(juce::StreamingSocket& client, const juce::String& line)
{
    juce::String msg = line + "\n";
    juce::CharPointer_UTF8 utf8 = msg.toUTF8();
    client.write(utf8.getAddress(), static_cast<int>(::strlen(utf8.getAddress())));
}

void JsonServer::handleClient(juce::StreamingSocket& client)
{
    juce::String buffer;

    while (!threadShouldExit())
    {
        // waitUntilReady: 1=readable/EOF, 0=timeout, -1=error
        int ready = client.waitUntilReady(true, 100);

        if (ready < 0)
            break;

        if (ready == 0)
            continue;

        char chunk[4096];
        int bytes = client.read(chunk, sizeof(chunk) - 1, false);

        if (bytes <= 0)  // 0=EOF (client closed), <0=error
            break;

        chunk[bytes] = '\0';
        buffer += juce::String::fromUTF8(chunk, bytes);

        int pos;
        while ((pos = buffer.indexOfChar('\n')) >= 0)
        {
            juce::String line = buffer.substring(0, pos).trim();
            buffer = buffer.substring(pos + 1);

            if (line.isEmpty())
                continue;

            juce::var json = juce::JSON::parse(line);
            if (json.isVoid())
                continue;

            juce::ScopedLock lock(lastJsonLock);
            lastJson = line;

            // Dispatch: synchronous handlers for query / load_syx
            juce::DynamicObject* obj = json.getDynamicObject();
            if (obj == nullptr)
                continue;

            // command: {"query": "all"}
            if (obj->hasProperty("query"))       { handleQuery(client);            continue; }

            // Handle "render" command: {"render": "/path/to/output.wav"}
            // Optional fields: "midi_note" (int, default 60), "velocity" (float 0-1, default 0.8)
            if (obj->hasProperty("render"))      { handleRender(client, json);     continue; }

            // command: {"load_syx": "/path.syx", "program": 0}
            if (obj->hasProperty("load_syx"))    { handleLoadSyx(client, json);    continue; }

            // command: {"set_program": 3}
            if (obj->hasProperty("set_program")) { handleSetProgram(client, json); continue; }

            // command:   {"batch": [...], "midi_note": 69, "velocity": 0.8}
            // Each item: {"output": "/out/001.wav", "params": {...}}
            //            {"output": "/out/002.wav", "set_program": 5}
            //            {"output": "/out/003.wav", "load_syx": "/path.syx", "program": 3}
            if (obj->hasProperty("batch"))       { handleBatch(client, json);      continue; }

            // Apply params on the message thread and wait for CtrlUpdate
            // callAsync messages (queued by setParameter) to finish too.
            auto done = std::make_shared<juce::WaitableEvent>();
            juce::var captured = json;
            juce::MessageManager::callAsync([this, captured, done]() mutable {
                applyJson(captured);
                juce::MessageManager::callAsync([done]() mutable {
                    done->signal();
                });
            });
            done->wait(5000);
            sendJson(client, R"({"ok": true})");
        }
    }
}

// Return all normalized parameter values as JSON.
void JsonServer::handleQuery(juce::StreamingSocket& client)
{
    juce::DynamicObject::Ptr obj = new juce::DynamicObject();
    int n = processor.getNumParameters();
    for (int i = 0; i < n; ++i)
        obj->setProperty(processor.getParameterName(i), processor.getParameter(i));

    sendJson(client, juce::JSON::toString(juce::var(obj.get()), true));
}

// Render a clip with the current settings.
void JsonServer::handleRender(juce::StreamingSocket& client, const juce::var& json)
{
    juce::DynamicObject* obj = json.getDynamicObject();
    juce::String outputPath = obj->getProperty("render").toString();
    if (outputPath.isEmpty())
    {
        return;
    }

    int   midiNote = obj->hasProperty("midi_note")
                        ? (int)obj->getProperty("midi_note")
                        : 60;
    float velocity = obj->hasProperty("velocity")
                        ? juce::jlimit(0.0f, 1.0f, (float)obj->getProperty("velocity"))
                        : 0.8f;

    juce::File outFile(outputPath);
    processor.renderClipToFile(outFile, midiNote, velocity);

    if (outFile.existsAsFile() && outFile.getSize() > 0)
        sendJson(client, R"({"ok": true})");
    else
        sendJson(client, R"({"error": "file write failed"})");
}

// Load a .syx file and apply it to the processor.
void JsonServer::handleLoadSyx(juce::StreamingSocket& client, const juce::var& json)
{
    juce::DynamicObject* obj = json.getDynamicObject();
    juce::String path = obj->getProperty("load_syx").toString();
    int program = obj->hasProperty("program")
        ? juce::jlimit(0, 31, (int)obj->getProperty("program"))
        : 0;

    juce::File file(path);
    if (!file.existsAsFile()) {
        sendJson(client, R"({"error": "file not found"})");
        return;
    }

    Cartridge cart;
    if (cart.load(file) == -1) {
        sendJson(client, R"({"error": "failed to load file"})");
        return;
    }

    // Apply on the message thread, then wait for completion (max 5 s).
    std::shared_ptr<juce::WaitableEvent> done = std::make_shared<juce::WaitableEvent>();
    juce::MessageManager::callAsync([this, cart, program, done]() mutable {
        processor.loadCartridge(cart);
        processor.setCurrentProgram(program);
        processor.forceRefreshUI = true;
        processor.triggerAsyncUpdate();
        done->signal();
    });
    done->wait(5000);

    // Build success response including the program name.
    juce::DynamicObject::Ptr resp = new juce::DynamicObject();
    resp->setProperty("ok", true);
    resp->setProperty("program", program);
    resp->setProperty("program_name", processor.getProgramName(program));
    sendJson(client, juce::JSON::toString(juce::var(resp.get()), true));
}

// Switch to a program in the current cartridge.
void JsonServer::handleSetProgram(juce::StreamingSocket& client, const juce::var& json)
{
    juce::DynamicObject* obj = json.getDynamicObject();
    int program = juce::jlimit(0, 31, (int)obj->getProperty("set_program"));

    auto done = std::make_shared<juce::WaitableEvent>();
    juce::MessageManager::callAsync([this, program, done]() {
        processor.setCurrentProgram(program);
        processor.forceRefreshUI = true;
        processor.triggerAsyncUpdate();
        done->signal();
    });
    done->wait(5000);

    juce::DynamicObject::Ptr resp = new juce::DynamicObject();
    resp->setProperty("ok", true);
    resp->setProperty("program", program);
    resp->setProperty("program_name", processor.getProgramName(program));
    sendJson(client, juce::JSON::toString(juce::var(resp.get()), true));
}

// Handle a batch of render requests.
void JsonServer::handleBatch(juce::StreamingSocket& client, const juce::var& json)
{
    juce::DynamicObject* topObj = json.getDynamicObject();
    const juce::var& batchVar = topObj->getProperty("batch");
    if (!batchVar.isArray())
    {
        sendJson(client, R"({"error": "'batch' must be an array"})");
        return;
    }

    const juce::Array<juce::var>& items = *batchVar.getArray();

    const int   defNote  = topObj->hasProperty("midi_note")
                             ? juce::jlimit(0, 127, (int)topObj->getProperty("midi_note")) : 60;
    const float defVel   = topObj->hasProperty("velocity")
                             ? juce::jlimit(0.0f, 1.0f, (float)topObj->getProperty("velocity")) : 0.8f;
    const int   total    = (int)items.size();
    int         succeeded = 0;
    juce::Array<juce::var> failures;

    for (int idx = 0; idx < total; ++idx)
    {
        juce::DynamicObject* itemObj = items[idx].getDynamicObject();
        if (itemObj == nullptr) continue;

        juce::String outPath = itemObj->getProperty("output").toString();
        if (outPath.isEmpty())
        {
            juce::DynamicObject::Ptr f = new juce::DynamicObject();
            f->setProperty("index", idx);  f->setProperty("error", "missing 'output'");
            failures.add(juce::var(f.get()));  continue;
        }

        // Load .syx on the server thread to avoid blocking the message thread with file I/O.
        std::shared_ptr<Cartridge> cartPtr;
        int cartProg = 0;
        if (itemObj->hasProperty("load_syx"))
        {
            juce::String syxPath = itemObj->getProperty("load_syx").toString();
            cartProg = itemObj->hasProperty("program")
                         ? juce::jlimit(0, 31, (int)itemObj->getProperty("program")) : 0;
            std::shared_ptr<Cartridge> c = std::make_shared<Cartridge>();
            if (!juce::File(syxPath).existsAsFile() || c->load(juce::File(syxPath)) == -1)
            {
                juce::DynamicObject::Ptr f = new juce::DynamicObject();
                f->setProperty("index", idx);  f->setProperty("error", "syx load failed");
                failures.add(juce::var(f.get()));  continue;
            }
            cartPtr = std::move(c);
        }

        const int setProg = itemObj->hasProperty("set_program")
                              ? juce::jlimit(0, 31, (int)itemObj->getProperty("set_program")) : -1;

        // Apply state on the message thread, then wait.
        std::shared_ptr<juce::WaitableEvent> done = std::make_shared<juce::WaitableEvent>();
        juce::var itemCopy = items[idx];
        juce::MessageManager::callAsync([this, cartPtr, cartProg, setProg, itemCopy, done]() mutable {
            if (cartPtr)
            {
                processor.loadCartridge(*cartPtr);
                processor.setCurrentProgram(cartProg);
            }
            else if (setProg >= 0)
            {
                processor.setCurrentProgram(setProg);
            }

            if (juce::DynamicObject* obj = itemCopy.getDynamicObject())
            {
                if (juce::DynamicObject* paramsObj = obj->getProperty("params").getDynamicObject())
                {
                    for (auto& prop : paramsObj->getProperties())
                    {
                        auto it = paramToIndex.find(prop.name.toString().toStdString());
                        if (it != paramToIndex.end())
                            processor.setParameter(it->second,
                                juce::jlimit(0.0f, 1.0f, (float)prop.value));
                    }
                }
            }

            processor.forceRefreshUI = true;
            processor.triggerAsyncUpdate();
            done->signal();
        });
        done->wait(5000);

        // Render directly on the server thread (renderLock keeps the audio thread out).
        const int   note = itemObj->hasProperty("midi_note")
                             ? juce::jlimit(0, 127, (int)itemObj->getProperty("midi_note")) : defNote;
        const float vel  = itemObj->hasProperty("velocity")
                             ? juce::jlimit(0.0f, 1.0f, (float)itemObj->getProperty("velocity")) : defVel;

        juce::File outFile(outPath);
        processor.renderClipToFile(outFile, note, vel);

        if (outFile.existsAsFile() && outFile.getSize() > 0)
        {
            ++succeeded;
        }
        else
        {
            juce::DynamicObject::Ptr f = new juce::DynamicObject();
            f->setProperty("index", idx);  f->setProperty("error", "file write failed");
            failures.add(juce::var(f.get()));
        }
    }

    juce::DynamicObject::Ptr resp = new juce::DynamicObject();
    resp->setProperty("batch_done", true);
    resp->setProperty("total",      total);
    resp->setProperty("succeeded",  succeeded);
    resp->setProperty("failed",     (int)failures.size());
    if (!failures.isEmpty())
        resp->setProperty("failures", juce::var(failures));
    sendJson(client, juce::JSON::toString(juce::var(resp.get()), true));
}