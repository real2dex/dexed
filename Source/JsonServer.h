// Copyright (C) 2026 Saisana299
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "../JuceLibraryCode/JuceHeader.h"
#include <unordered_map>

class DexedAudioProcessor;

class JsonServer : public juce::Thread {

public:
    static constexpr int DEFAULT_PORT = 8765;
    static constexpr int MAX_PORT_TRIES = 16;

    JsonServer(DexedAudioProcessor& processor);
    ~JsonServer() override;

    void run() override;

    int getPort() const { return port.load(); }
    bool getIsListening() const { return isListening.load(); }
    int getClients() const { return clients.load(); }

    juce::String getLastJson() const {
        juce::ScopedLock lock(lastJsonLock);
        return lastJson;
    };

private:
    DexedAudioProcessor& processor;
    juce::StreamingSocket server;
    std::unordered_map<std::string, int> paramToIndex;

    std::atomic<bool> isListening { false };
    std::atomic<int> clients { 0 };
    std::atomic<int> port { 0 };

    mutable juce::CriticalSection lastJsonLock;
    juce::String lastJson;

    void applyJson(const juce::var& json);
    static void sendJson(juce::StreamingSocket& client, const juce::String& line);

    void handleClient(juce::StreamingSocket& client);
    void handleQuery(juce::StreamingSocket& client);
    void handleRender(juce::StreamingSocket& client, const juce::var& json);
    void handleLoadSyx(juce::StreamingSocket& client, const juce::var& json);
    void handleSetProgram(juce::StreamingSocket& client, const juce::var& json);
    void handleBatch(juce::StreamingSocket& client, const juce::var& json);
};