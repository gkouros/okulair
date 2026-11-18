/*
    SPDX-FileCopyrightText: 2026 Georgios Kouros
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once
#include "provider.h"
namespace OkulAIr
{
struct ServicePreset { QString name; QString baseUrl; Protocol protocol; bool requiresKey; };
inline QList<ServicePreset> servicePresets()
{
    // Keep the first three indices stable for saved settings from v1.
    return {{QStringLiteral("Local / custom compatible server"), QStringLiteral("http://localhost:11434/v1"), Protocol::Compatible, false},
            {QStringLiteral("OpenAI (API key)"), QStringLiteral("https://api.openai.com/v1"), Protocol::OpenAI, true},
            {QStringLiteral("Anthropic / Claude (API key)"), QStringLiteral("https://api.anthropic.com/v1"), Protocol::Anthropic, true},
            {QStringLiteral("ChatGPT (subscription sign-in)"), QStringLiteral("https://api.openai.com/v1"), Protocol::ChatGPT, false},
            {QStringLiteral("DeepSeek (API key)"), QStringLiteral("https://api.deepseek.com"), Protocol::Compatible, true},
            {QStringLiteral("Google Gemini (API key)"), QStringLiteral("https://generativelanguage.googleapis.com/v1beta/openai"), Protocol::Compatible, true},
            {QStringLiteral("Groq (API key)"), QStringLiteral("https://api.groq.com/openai/v1"), Protocol::Compatible, true},
            {QStringLiteral("Mistral (API key)"), QStringLiteral("https://api.mistral.ai/v1"), Protocol::Compatible, true},
            {QStringLiteral("OpenRouter (API key)"), QStringLiteral("https://openrouter.ai/api/v1"), Protocol::Compatible, true},
            {QStringLiteral("xAI / Grok (API key)"), QStringLiteral("https://api.x.ai/v1"), Protocol::Compatible, true},
            {QStringLiteral("Gemini (Google account via official CLI)"), QStringLiteral("https://gemini.google.com"), Protocol::GeminiAccount, false},
            {QStringLiteral("GitHub Copilot (account via official CLI)"), QStringLiteral("https://github.com"), Protocol::CopilotAccount, false},
            {QStringLiteral("Qwen (subscription via official CLI)"), QStringLiteral("https://chat.qwen.ai"), Protocol::QwenAccount, false}};
}
}
