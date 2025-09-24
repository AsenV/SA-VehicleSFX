// VehicleSFX_ASI.cpp
// Versão atualizada: comporta pausa do jogo
// Requer plugin-sdk, FMOD Core, C++17

#include "plugin.h"
#include "CAEVehicleAudioEntity.h"
#include "CAESound.h"
#include "CMessages.h"
#include "CCamera.h"
#include "CPad.h"
#include "fmod.hpp"
#include "CMenuManager.h"
#include "CFileLoader.h"
#include "CSprite2d.h"
#include "CAudioEngine.h"
#include "rwcore.h"  

#include <filesystem>
#include <map>
#include <string>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cstring>
#include <fstream>
#include <ctime>
#include <cstdarg>

using namespace plugin;
namespace fs = std::filesystem;

// ---------------- params ----------------

// Parâmetros ajustáveis (valores seguros por padrão)
// array simples, NÃO constexpr
// ---- inicialize o array com defaults seguros ----
static float START_PITCH_PER_GEAR[6] = { 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f }; static float TARGET_PITCH;
static float PITCH_SMOOTHING;
static float PITCH_AMPLIFY_MAX;
static float MAX_OVERSHOOT;
static float DECEL_FACTOR;
static float ACCEL_SPEED_MULT;    // velocidade de retorno ao acelerar
static float DECEL_SPEED_MULT;    // velocidade de queda ao desacelerar (reduzido)
static float MIN_PITCH;          // não deixar o pitch abaixo disso

static float BASE_START_DROP; // ajusta conforme preferir
static float BASE_SHIFT_DROP;         // ajuste: negative => pitch goes down (engrossa)
static float EXTRA_DROP_PER_GEAR;       // quanto mais nas marchas altas
static int SHIFT_DROP_DURATION_MS; // duração do decay (tune aqui)

static float WIND_MAX_VOL;        // volume máximo do wind
static float WIND_SPEED_SCALE;    // velocidade onde wind chega a 1.0
static float WIND_FADE_MS;      // duração do fade-in inicial (ms)
static float MAX_WIND_RATE_PER_SEC; // quanta fração de volume pode mudar por segundo
static float WIND_STOP_THRESHOLD; // abaixo disto paramos o canal


// ---------------- logging ----------------
static void InitLog() {
    std::ofstream f("VehicleSFX_log.txt", std::ios::trunc); // limpa o arquivo
    if (f.is_open()) {
        std::time_t t = std::time(nullptr);
        std::string ts = std::asctime(std::localtime(&t));
        if (!ts.empty() && ts.back() == '\n') ts.pop_back();
        f << ts << " : Log started" << std::endl;
        f.close();
    }
}

static void WriteLog(const char* fmt, ...) {
    char buf[1024];
    va_list ap; va_start(ap, fmt); vsnprintf(buf, sizeof(buf), fmt, ap); va_end(ap);
    std::ofstream f("VehicleSFX_log.txt", std::ios::app);
    if (f.is_open()) {
        std::time_t t = std::time(nullptr);
        std::string ts = std::asctime(std::localtime(&t));
        if (!ts.empty() && ts.back() == '\n') ts.pop_back();
        f << ts << " : " << buf << std::endl;
        f.close();
    }
}

// ---------------- config ----------------
static std::map<std::string, float> g_config;
// ---- coloque isto no topo (utilitários) ----
static inline std::string Trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && std::isspace((unsigned char)s[a])) ++a;
    size_t b = s.size();
    while (b > a && std::isspace((unsigned char)s[b - 1])) --b;
    return s.substr(a, b - a);
}
static inline std::string ToLower(const std::string& s) {
    std::string out; out.reserve(s.size());
    for (char c : s) out.push_back((char)std::tolower((unsigned char)c));
    return out;
}


// ---- substitua LoadConfig / GetConfig por isto ----
static void LoadConfig(const std::string& path) {
    g_config.clear();
    std::ifstream f(path);
    if (!f.is_open()) {
        WriteLog("LoadConfig: arquivo %s não encontrado", path.c_str());
        return;
    }

    std::string line;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        // strip UTF BOM (se houver)
        if (line.size() >= 3 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF) {
            line = line.substr(3);
        }
        // comentários/sections
        std::string t = Trim(line);
        if (t.empty()) continue;
        if (t[0] == ';' || t[0] == '[') continue;

        auto eq = line.find('=');
        if (eq == std::string::npos) continue;

        std::string key = Trim(line.substr(0, eq));
        std::string val = Trim(line.substr(eq + 1));

        if (key.empty() || val.empty()) continue;

        try {
            float fv = std::stof(val);
            // normaliza chave para lowercase (evita problemas de espaços/case)
            key = ToLower(key);
            g_config[key] = fv;
        }
        catch (const std::exception& e) {
            WriteLog("LoadConfig: failed to parse '%s'='%s' (%s)", key.c_str(), val.c_str(), e.what());
        }
    }
    f.close();
    WriteLog("LoadConfig: carregado %zu entradas", g_config.size());
}

static float GetConfig(const std::string& key, float def) {
    std::string k = ToLower(key);
    auto it = g_config.find(k);
    return (it != g_config.end()) ? it->second : def;
}

void InitParams() {
    for (int gear = 1; gear <= 5; gear++) {
        std::string key = "StartPitchGear" + std::to_string(gear);
        START_PITCH_PER_GEAR[gear] = GetConfig(key, START_PITCH_PER_GEAR[gear]);
        WriteLog("InitParams: %s = %.3f", key.c_str(), START_PITCH_PER_GEAR[gear]);
    }

    TARGET_PITCH = GetConfig("TargetPitch", 1.0f);
    PITCH_SMOOTHING = GetConfig("PitchSmoothing", 5.5f);
    PITCH_AMPLIFY_MAX = GetConfig("PitchAmplifyMax", 1.01f);
    MAX_OVERSHOOT = GetConfig("MaxOvershoot", 1.08f);
    DECEL_FACTOR = GetConfig("DecelFactor", 0.8f);
    ACCEL_SPEED_MULT = GetConfig("AccelSpeedMult", 1.2f);
    DECEL_SPEED_MULT = GetConfig("DecelSpeedMult", 1.8f);
    MIN_PITCH = GetConfig("MinPitch", 0.5f);
    BASE_START_DROP = GetConfig("BaseStartDrop", -0.06f);
    BASE_SHIFT_DROP = GetConfig("BaseShiftDrop", -0.1f);
    EXTRA_DROP_PER_GEAR = GetConfig("ExtraDropPerGear", 0.6f);
    SHIFT_DROP_DURATION_MS = (int)GetConfig("ShiftDropDurationMs", 1000);
    WIND_MAX_VOL = GetConfig("WindMaxVolume", 0.75f);
    WIND_SPEED_SCALE = GetConfig("WindSpeedScale", 60.0f);
    WIND_FADE_MS = GetConfig("WindFadeMs", 2500.0f);
    MAX_WIND_RATE_PER_SEC = GetConfig("MaxWindRatePerSec", 0.25f);
    WIND_STOP_THRESHOLD = GetConfig("WindStopThreshold", 0.1f);
}

// --- globals para o logo FMOD ---
static RwTexDictionary* g_logoTxd = nullptr;
static RwTexture* g_logoTex = nullptr;
static bool g_fmodLogoLoaded = false;

// ---------------- globals ----------------
static FMOD::System* g_fmodCore = nullptr;
static std::mutex g_mutex;
static std::string g_basePath = PLUGIN_PATH("vsfx");
static bool g_gamePaused = false; // estado local de pausa

static const char* s_names[] = {
    "idle.wav",
    "engine.wav",
    "wind.wav",
    "shiftup.wav",
    "shiftdn.wav",
    "backfire.wav"
};

struct WavBank { std::map<std::string, FMOD::Sound*> sounds; };

enum LoopMode { LM_NONE = 0, LM_IDLE, LM_GEAR };

struct VehicleAudioInstance {
    CVehicle* vehicle = nullptr;

    // canais FMOD que o ASI pode controlar
    FMOD::Channel* loopChannel = nullptr;         // loop actual (gear / idle)
    FMOD::Channel* pendingLoopChannel = nullptr;  // canal pendente (não usado agressivamente nesta versão)
    FMOD::Channel* attackChannel = nullptr;       // attack (one-shot)
    FMOD::Channel* shiftChannel = nullptr;        // alias
    FMOD::Channel* windChannel = nullptr;   // canal para wind.wav

    // vento
    float currentWindVolume = 0.0f;         // volume atual do wind
    float targetWindVolume = 0.0f;          // target smoothed

    // stored volumes para pause/resume
    float storedLoopVolume = 0.0f;
    float storedWindVolume = 0.0f;
    float storedAttackVolume = 0.0f;

    // estado / meta
    int lastGear = INT_MIN;
    int pendingGear = -1;
    bool inShift = false;
    unsigned int attackEndTimeMs = 0;

    // meta dados de controlo/volume/pitch
    float lastSpeed = 0.0f;
    float currentPitch = 1.0f;
    float currentVolume = 0.0f;

    // banco de sons e flags
    WavBank* bank = nullptr;
    bool mutedGameAudio = false; // se já silenciámos o audio da engine
    float storedVolume = 0.0f;

    // modos de motor p/ comportamento de pitch
    enum EngineMode { EM_NONE = 0, EM_ACCEL = 1, EM_DECEL = 2 };
    EngineMode engineMode = EM_NONE;
    float desiredEnginePitch = 1.0f; // alvo atual (aceleração / desaceleração)

    // transient shift drop (negativo = engrossa), decai com o tempo
    float shiftPitchDrop = 0.0f;
    unsigned int shiftStartMs = 0;

    // membros extras usados no código
    LoopMode loopMode = LM_NONE;
    bool wasAccelerating = false;
    unsigned int lastAccelReleaseMs = 0;
    unsigned int lastShiftUpMs = 0;
    unsigned int lastShiftDnMs = 0;
    unsigned int lastBackfireMs = 0;

    // timestamp de quando o wind foi (re)criado / iniciado para fazer fade-in
    unsigned int windStartMs = 0;
};

// caches
static std::map<int, WavBank*> g_modelBanks;
static std::map<CVehicle*, VehicleAudioInstance> g_vehicleInstances;

// ---------------- FMOD helpers ----------------
static FMOD::System* GetCoreSystem() { return g_fmodCore; }

static FMOD::Sound* LoadWav(FMOD::System* core, const std::string& path, bool loop) {
    if (!core) return nullptr;
    FMOD::Sound* s = nullptr;
    FMOD_MODE mode = static_cast<FMOD_MODE>(FMOD_3D | FMOD_CREATESAMPLE);
    FMOD_RESULT r = core->createSound(path.c_str(), mode, nullptr, &s);
    if (r != FMOD_OK || !s) {
        WriteLog("LoadWav: createSound failed %s r=%d -> fallback", path.c_str(), (int)r);
        r = core->createSound(path.c_str(), static_cast<FMOD_MODE>(FMOD_CREATESAMPLE), nullptr, &s);
        if (r != FMOD_OK || !s) { WriteLog("LoadWav fallback failed %s r=%d", path.c_str(), (int)r); return nullptr; }
    }
    if (loop) { s->setMode(FMOD_LOOP_NORMAL); s->setLoopCount(-1); }
    else { s->setMode(FMOD_LOOP_OFF); s->setLoopCount(0); }
    WriteLog("LoadWav: loaded %s loop=%d", path.c_str(), loop ? 1 : 0);
    return s;
}

static WavBank* LoadBankForModel(int modelId) {
    std::lock_guard<std::mutex> lk(g_mutex);
    auto it = g_modelBanks.find(modelId);
    if (it != g_modelBanks.end()) return it->second;

    std::string folder = g_basePath + "\\" + std::to_string(modelId);
    WriteLog("LoadBankForModel: modelId=%d folder=%s", modelId, folder.c_str());
    if (!fs::exists(folder) || !fs::is_directory(folder)) {
        WriteLog("LoadBankForModel: folder not found %s", folder.c_str());
        g_modelBanks[modelId] = nullptr;
        return nullptr;
    }

    FMOD::System* core = GetCoreSystem();
    if (!core) return nullptr;

    WavBank* bank = new WavBank();
    for (const char* name : s_names) {
        std::string p = folder + "\\" + name;
        if (!fs::exists(p)) continue;
        bool loop = (strcmp(name, "idle.wav") == 0) || (strcmp(name, "engine.wav") == 0) || (strcmp(name, "wind.wav") == 0);
        FMOD::Sound* s = LoadWav(core, p, loop);
        if (s) {
            // Se for wind.wav, força modos 2D+loop para evitar atenuação 3D indesejada
            if (strcmp(name, "wind.wav") == 0) {
                try {
                    s->setMode(static_cast<FMOD_MODE>(FMOD_2D | FMOD_LOOP_NORMAL));
                    s->setLoopCount(-1);
                }
                catch (...) {}
            }
            std::string key = name;
            auto pos = key.rfind('.');
            if (pos != std::string::npos) key = key.substr(0, pos);
            bank->sounds[key] = s;
        }

    }
    g_modelBanks[modelId] = bank;
    WriteLog("LoadBankForModel: finished modelId=%d sounds=%d", modelId, (int)bank->sounds.size());
    return bank;
}

// Se o jogo está pausado, não devemos iniciar novos canais
static inline bool IsGamePaused() {
    return CTimer::m_UserPause != 0;
}

static FMOD::Channel* PlayLoop(CVehicle* veh, FMOD::Sound* snd, float initVol, float initPitch) {
    if (IsGamePaused()) {
        // não iniciar loops durante pausa
        return nullptr;
    }
    FMOD::System* core = GetCoreSystem();
    if (!core || !snd) return nullptr;
    FMOD::Channel* ch = nullptr;
    FMOD_RESULT r = core->playSound(snd, nullptr, true, &ch);
    if (r != FMOD_OK || !ch) { WriteLog("PlayLoop failed r=%d", (int)r); return nullptr; }
    CVector p = veh->GetPosition();
    FMOD_VECTOR fv = { p.x, p.y, p.z };
    FMOD_VECTOR vel = { veh->m_vecMoveSpeed.x, veh->m_vecMoveSpeed.y, veh->m_vecMoveSpeed.z };
    try { ch->set3DAttributes(&fv, &vel); }
    catch (...) {}
    try { ch->set3DMinMaxDistance(1.0f, 300.0f); }
    catch (...) {}
    try { ch->setVolume(initVol); }
    catch (...) {}
    try { ch->setPitch(initPitch); }
    catch (...) {}
    try { ch->setPaused(false); }
    catch (...) {}
    return ch;
}

static FMOD::Channel* PlayOneShot(CVehicle* veh, FMOD::Sound* snd, float pitch = 1.0f, float volume = 1.0f) {
    if (IsGamePaused()) {
        // não iniciar one-shots durante pausa
        return nullptr;
    }
    FMOD::System* core = GetCoreSystem();
    if (!core || !snd) return nullptr;
    try { snd->setMode(FMOD_LOOP_OFF); snd->setLoopCount(0); }
    catch (...) {}
    FMOD::Channel* ch = nullptr;
    FMOD_RESULT r = core->playSound(snd, nullptr, true, &ch);
    if (r != FMOD_OK || !ch) { WriteLog("PlayOneShot playSound failed r=%d", (int)r); return nullptr; }
    try { ch->setLoopCount(0); ch->setMode(FMOD_LOOP_OFF); }
    catch (...) {}
    CVector p = veh->GetPosition();
    FMOD_VECTOR fv = { p.x, p.y, p.z };
    FMOD_VECTOR vel = { veh->m_vecMoveSpeed.x, veh->m_vecMoveSpeed.y, veh->m_vecMoveSpeed.z };
    try { ch->set3DAttributes(&fv, &vel); }
    catch (...) {}
    // importante: define min/max distance para evitar atenuação indesejada
    try { ch->set3DMinMaxDistance(1.0f, 300.0f); }
    catch (...) {}
    try { ch->setPitch(pitch); ch->setVolume(volume); ch->setPaused(false); }
    catch (...) {}
    return ch;
}


static void StopChannelSafe(FMOD::Channel*& ch) {
    if (!ch) return;
    try { ch->stop(); }
    catch (...) {}
    ch = nullptr;
}

static unsigned int GetSoundLengthMs(FMOD::Sound* s) {
    if (!s) return 150;
    unsigned int len = 150;
    if (s->getLength(&len, FMOD_TIMEUNIT_MS) != FMOD_OK) return 150;
    return len;
}

// ---------------- Mute built-in vehicle audio ----------------
static void MuteGameVehicleAudio(CVehicle* veh) {
    if (!veh) return;
    CAEVehicleAudioEntity& va = veh->m_vehicleAudio;
    WriteLog("MuteGameVehicleAudio: model=%d engineState=%d accelBank=%d decelBank=%d",
        veh->m_nModelIndex, (int)va.m_nEngineState, (int)va.m_nEngineAccelerateSoundBankId, (int)va.m_nEngineDecelerateSoundBankId);
    va.m_nEngineState = 5;
    va.m_bSoundsStopped = true;
    va.m_nEngineAccelerateSoundBankId = (short)-1;
    va.m_nEngineDecelerateSoundBankId = (short)-1;
    va.m_nEngineBankSlotId = (short)-1;
    for (int i = 0; i < 12; ++i) {
        if (va.m_aEngineSounds[i].m_pSound) {
            try { va.m_aEngineSounds[i].m_pSound->StopSoundAndForget(); }
            catch (...) {}
            va.m_aEngineSounds[i].m_pSound = nullptr;
        }
    }
    if (va.m_pRoadNoiseSound) { try { va.m_pRoadNoiseSound->StopSoundAndForget(); } catch (...) {} va.m_pRoadNoiseSound = nullptr; }
    WriteLog("MuteGameVehicleAudio: done model=%d", veh->m_nModelIndex);
}

// ---------------- thresholds ----------------
static const float IDLE_SPEED_THRESHOLD = 0.01f; // detecta movimento cedo
static const float PAD_ACCEL_THRESHOLD_SHORT = 10; // threshold para pad
static const float GASPEDAL_ACCEL_THRESHOLD = 0.05f; // veh->m_fGasPedal threshold

// ---------------- cooldowns ----------------
static const unsigned int COOLDOWN_SHIFT_MS = 200;
static const unsigned int COOLDOWN_BACKFIRE_MS = 600;

// ---------------- helpers ----------------
static bool IsVehicleValidForAudio(CVehicle* veh) {
    if (!veh) return false;
    if (!IsVehiclePointerValid(veh)) return false;
    if (veh->bIsDrowning) return false;
    if (veh->m_fHealth <= 0.0f) return false;
    if (!veh->CanBeDriven()) return false;
    return true;
}

static void EnsureLoopPlaying(VehicleAudioInstance& inst, const std::string& loopKey, int gearForPitch) {
    CVehicle* veh = inst.vehicle;
    if (!veh || !inst.bank) return;

    bool already = (inst.loopMode == LM_IDLE && loopKey == "idle") || (inst.loopMode == LM_GEAR && loopKey == "engine");
    if (already && inst.loopChannel) return;

    StopChannelSafe(inst.loopChannel);

    auto it = inst.bank->sounds.find(loopKey);
    if (it == inst.bank->sounds.end()) {
        WriteLog("EnsureLoopPlaying: missing '%s' for model=%d", loopKey.c_str(), veh->m_nModelIndex);
        inst.loopMode = LM_NONE;
        return;
    }
    FMOD::Sound* s = it->second;

    int gIndex = std::clamp(gearForPitch <= 0 ? 1 : gearForPitch, 1, 5);
    float startPitch = START_PITCH_PER_GEAR[gIndex];

    inst.loopChannel = PlayLoop(veh, s, inst.currentVolume, startPitch);
    if (!inst.loopChannel) {
        inst.loopMode = LM_NONE;
        WriteLog("EnsureLoopPlaying: not started (possibly paused) '%s' model=%d", loopKey.c_str(), veh->m_nModelIndex);
        return;
    }
    inst.currentPitch = startPitch;
    inst.loopMode = (loopKey == "idle") ? LM_IDLE : LM_GEAR;

    WriteLog("EnsureLoopPlaying: started '%s' model=%d gear=%d pitch=%.2f", loopKey.c_str(), veh->m_nModelIndex, gIndex, startPitch);
}

static void PlayOverlayOnceIfReady(VehicleAudioInstance& inst, const std::string& key, unsigned int& lastMs, unsigned int cooldownMs) {
    CVehicle* veh = inst.vehicle;
    if (!veh || !inst.bank) return;
    unsigned int now = CTimer::m_snTimeInMilliseconds;
    if ((now - lastMs) < cooldownMs) return;
    auto it = inst.bank->sounds.find(key);
    if (it == inst.bank->sounds.end()) return;
    FMOD::Sound* s = it->second;
    if (IsGamePaused()) return;
    FMOD::Channel* ch = PlayOneShot(veh, s, 1.0f, 1.0f);
    if (ch) {
        // armazena o channel para podermos parar/mutar mais tarde
        if (key == "shiftup" || key == "shiftdn") inst.shiftChannel = ch;
        else inst.attackChannel = ch;
    }
    lastMs = now;
    WriteLog("PlayOverlayOnceIfReady: played '%s' for model=%d", key.c_str(), veh->m_nModelIndex);
}

// helper: tenta carregar a txd/texture (chame isto em InitFMOD)
static void LoadFMODLogo() {
    try {
        // Caminho: coloca fmod.txd (ou altera o caminho abaixo)
        const char* path = PLUGIN_PATH((char*)"fmod.txd");
        g_logoTxd = CFileLoader::LoadTexDictionary(path);
        if (g_logoTxd) {
            // GetFirstTexture é usado no mod de exemplo — se não existir, tenta obter direto.
            extern RwTexture* GetFirstTexture(RwTexDictionary*); // se não tiveres, remove e implemente o teu getter
            g_logoTex = GetFirstTexture(g_logoTxd);
            if (g_logoTex) {
                g_fmodLogoLoaded = true;
                WriteLog("LoadFMODLogo: loaded logo.txd");
            }
            else {
                WriteLog("LoadFMODLogo: txd carregado mas texture não encontrada");
            }
        }
        else {
            WriteLog("LoadFMODLogo: logo.txd nao encontrado em %s", path);
        }
    }
    catch (...) {
        WriteLog("LoadFMODLogo: excecao ao carregar logo.txd");
        g_logoTxd = nullptr;
        g_logoTex = nullptr;
        g_fmodLogoLoaded = false;
    }
}

// helper: desenha o logo em tela (invocado via drawMenuBackgroundEvent)
static void DrawFMODLogoIfNeeded() {
    if (!g_fmodLogoLoaded) return;
    try {
        // desenha somente na página de configurações de áudio
        if (FrontEndMenuManager.m_nCurrentMenuPage == eMenuPage::MENUPAGE_SOUND_SETTINGS) {
            // configura a textura
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, g_logoTex->raster);

            // Ajusta a posição/escala do quadrado (mude coordenadas conforme quiseres)
            CRGBA color = CRGBA(255, 255, 255, 255);
            float logoWidth = 128.0f;
            float logoHeight = 64.0f;

            float posX = SCREEN_COORD_LEFT(50.0f);
            float posY = SCREEN_COORD_BOTTOM(150.0f); // altura de onde começar

            CSprite2d::SetVertices(
                CRect(posX,
                    posY,
                    posX + logoWidth,
                    posY + logoHeight),
                color, color, color, color
            );

            // desenha a quad texturizada
            RwIm2DRenderPrimitive(rwPRIMTYPETRIFAN, CSprite2d::maVertices, 4);

            // resetar textura
            RwRenderStateSet(rwRENDERSTATETEXTURERASTER, 0);

            // Opcional: desenhar texto de crédito abaixo do logo (se quiseres)
            // Exemplo simples usando DrawRect/CSprite: se preferir texto real, podes usar CFont.
            // Here we keep it minimal (logo only) to evitar dependências em CFont.
        }
    }
    catch (...) {
        // ignora erros de RW/FMOD drawing
    }
}

// ---------------- Update per-vehicle ----------------
static void UpdateInstance(VehicleAudioInstance& inst) {
    CVehicle* veh = inst.vehicle;
    if (!veh) return;

    // Se veículo não é válido para audio -> pára canais e sinaliza sem loop
    if (!IsVehicleValidForAudio(veh)) {
        StopChannelSafe(inst.loopChannel);
        StopChannelSafe(inst.pendingLoopChannel);
        StopChannelSafe(inst.attackChannel);
        StopChannelSafe(inst.shiftChannel);
        StopChannelSafe(inst.windChannel);
        inst.loopMode = LM_NONE;
        return;
    }

    // read inputs — só pega input do jogador se o jogador estiver dentro deste veículo
    CPad* pad = CPad::GetPad(0);
    short padAccel = 0;
    CVehicle* playerVeh = FindPlayerVehicle(-1, true);
    bool padPressed = false;
    if (veh == playerVeh && pad) {
        padAccel = pad->GetAccelerate();
        padPressed = (padAccel > (short)PAD_ACCEL_THRESHOLD_SHORT);
    }
    unsigned int now = CTimer::m_snTimeInMilliseconds;

    bool vehGasPressed = (veh->m_fGasPedal > GASPEDAL_ACCEL_THRESHOLD);

    // detect accel release to create short window
    if (padPressed || vehGasPressed) {
        if (!inst.wasAccelerating) {
            inst.wasAccelerating = true;
            WriteLog("UpdateInstance: accel started model=%d", veh->m_nModelIndex);
        }
    }
    else {
        if (inst.wasAccelerating) {
            inst.wasAccelerating = false;
            inst.lastAccelReleaseMs = now;
            WriteLog("UpdateInstance: accel released model=%d at t=%u", veh->m_nModelIndex, now);
        }
    }

    // read speed & gear with safe access to offsets (mesma lógica tua)
    int gearNow = (int)veh->m_nCurrentGear;
    float speed = 0.0f;
    float gearMax = 1.0f;
    try {
        uintptr_t vehPtr = reinterpret_cast<uintptr_t>(veh);
        uintptr_t handlingPtrAddr = *(uintptr_t*)(vehPtr + 0x384);
        if (handlingPtrAddr) {
            uintptr_t transmissionPtr = handlingPtrAddr + 0x2C;
            speed = *(float*)(transmissionPtr + 0x64);
            uintptr_t gearEntry = transmissionPtr + (0x0C * (uintptr_t)std::max(1, gearNow));
            float maybe = *(float*)(gearEntry + 0x4);
            if (maybe > 0.0001f) gearMax = maybe;
        }
    }
    catch (...) { speed = 0.0f; gearMax = 1.0f; }

    // lazy load bank & mute once (permanece igual)
    if (!inst.bank) {
        inst.bank = LoadBankForModel(veh->m_nModelIndex);
        if (inst.bank && !inst.mutedGameAudio) { MuteGameVehicleAudio(veh); inst.mutedGameAudio = true; }
    }

    // gear change overlays (one-shot) + transient start-of-gear reset
    if (inst.lastGear == INT_MIN) inst.lastGear = gearNow;
    if (gearNow != inst.lastGear) {
        int oldGear = inst.lastGear;
        if (gearNow > oldGear) {
            PlayOverlayOnceIfReady(inst, "shiftup", inst.lastShiftUpMs, COOLDOWN_SHIFT_MS);
        }
        else {
            PlayOverlayOnceIfReady(inst, "shiftdn", inst.lastShiftDnMs, COOLDOWN_SHIFT_MS);
        }

        // transient: pequeno drop grave para dar "thump" na troca (negativo = engrossa)
        int gIdx = std::clamp(gearNow <= 0 ? 1 : gearNow, 1, 5);
        float gearFactorForDrop = float(gIdx - 1) / 4.0f; // 0..1

        float drop = BASE_SHIFT_DROP * (1.0f + gearFactorForDrop * EXTRA_DROP_PER_GEAR);
        inst.shiftPitchDrop = drop;
        inst.shiftStartMs = CTimer::m_snTimeInMilliseconds;

        // **IMPORTANTE**: reiniciar o pitch imediatamente para a base da marcha
        // — isso faz a sensação "começar do 0" por marcha.
        inst.currentPitch = START_PITCH_PER_GEAR[gIdx] + 0.15f * (inst.currentPitch - START_PITCH_PER_GEAR[gIdx]);
        inst.desiredEnginePitch = inst.currentPitch; // garante consistência com smoothing
        inst.lastGear = gearNow;

        WriteLog("Gear change: model=%d old=%d new=%d drop=%.3f startPitch=%.2f",
            veh->m_nModelIndex, oldGear, gearNow, drop, inst.currentPitch);
    }




    // --- backfire heuristic (por instância) ---
// usa inst.lastSpeed (persistente por veículo) ao invés de uma variável global
    float prevSpeed = inst.lastSpeed;
    float delta = speed - prevSpeed;

    // parâmetros ajustáveis
    constexpr float BACKFIRE_DELTA_THRESHOLD = -3.0f; // queda brusca em unidades de speed
    constexpr float BACKFIRE_RATIO_THRESHOLD = 0.35f; // requer velocidade relativa (proxy RPM)
    constexpr unsigned int RECENT_RELEASE_WINDOW_MS = 800u; // janela após soltar acelerador
    constexpr int BACKFIRE_CHANCE_HEAVY = 70; // % chance em queda brusca
    constexpr int BACKFIRE_CHANCE_RELEASE = 30; // % chance ao soltar acelerador
    constexpr int BACKFIRE_CHANCE_WHEELSPIN = 80; // % chance em wheelspin
    constexpr unsigned int COOLDOWN_WHEELSPIN_MS = COOLDOWN_BACKFIRE_MS / 2; // menor cooldown em burnout

    // calcula ratio como proxy de RPM (já calculado depois no pitch logic — reutilizamos)
    float ratio = (gearMax > 0.0001f) ? std::clamp(speed / gearMax, 0.0f, 1.0f) : 0.0f;

    bool wheelspin = (veh->m_fWheelSpinForAudio > 0.6f);
    bool heavyDrop = (delta < BACKFIRE_DELTA_THRESHOLD && ratio > BACKFIRE_RATIO_THRESHOLD);
    bool recentRelease = (inst.lastAccelReleaseMs != 0 && (now - inst.lastAccelReleaseMs) < RECENT_RELEASE_WINDOW_MS && ratio > 0.20f);

    if ((heavyDrop || recentRelease || wheelspin) && inst.bank) {
        if (inst.bank->sounds.count("backfire")) {
            // pseudo-random roll (determinístico por frame)
            unsigned int seed = (unsigned int)(now ^ (uintptr_t)veh);
            int roll = (int)(seed % 100);

            int chance = 0;
            unsigned int cooldown = COOLDOWN_BACKFIRE_MS;
            if (wheelspin) { chance = BACKFIRE_CHANCE_WHEELSPIN; cooldown = COOLDOWN_WHEELSPIN_MS; }
            else if (heavyDrop) chance = BACKFIRE_CHANCE_HEAVY;
            else if (recentRelease) chance = BACKFIRE_CHANCE_RELEASE;

            WriteLog("Backfire check model=%d prev=%.2f cur=%.2f delta=%.2f ratio=%.2f wheelspin=%.2f recentRel=%d roll=%d chance=%d",
                veh->m_nModelIndex, prevSpeed, speed, delta, ratio, veh->m_fWheelSpinForAudio, recentRelease ? 1 : 0, roll, chance);

            if (roll < chance) {
                // usa o mesmo helper (tem cooldown interno via inst.lastBackfireMs)
                PlayOverlayOnceIfReady(inst, "backfire", inst.lastBackfireMs, cooldown);
            }
        }
        else {
            WriteLog("Backfire missing for model=%d (folder=%s\\%d)", veh->m_nModelIndex, g_basePath.c_str(), veh->m_nModelIndex);
        }
    }

    // guardar speed por instância para próxima frame
    inst.lastSpeed = speed;


    // decide desired loop:
    // - se estamos acelerando (pad ou pedal) -> gear loop
    // - se estamos em movimento (velocidade > threshold) -> gear loop
    // - se parado -> idle
    bool padRecentlyReleased = (inst.lastAccelReleaseMs != 0 && (now - inst.lastAccelReleaseMs) < 1500u);
    bool isAccelerating = vehGasPressed || padPressed;
    bool wantGearLoop = (speed > IDLE_SPEED_THRESHOLD) || isAccelerating || (padRecentlyReleased && speed > 0.5f);

    // drop inicial
    if (gearNow == 1 && inst.lastGear == 1 && isAccelerating && inst.shiftPitchDrop == 0.0f) {
        inst.shiftPitchDrop = BASE_START_DROP;
        inst.shiftStartMs = CTimer::m_snTimeInMilliseconds;
        WriteLog("Initial first gear drop applied model=%d", veh->m_nModelIndex);
    }

    if (wantGearLoop) {
        EnsureLoopPlaying(inst, "engine", gearNow);
    }
    else {
        EnsureLoopPlaying(inst, "idle", gearNow);
    }

    // pitch/volume smoothing for active loop
    if (inst.loopChannel && inst.loopMode != LM_NONE) {
        float ratio = (gearMax > 0.0001f) ? std::clamp(speed / gearMax, 0.0f, 1.0f) : 0.0f;
        int gIndex = std::clamp(gearNow <= 0 ? 1 : gearNow, 1, 5);

        // update 3D attributes (prepara para usar tanto no loop quanto no wind)
        CVector pos = veh->GetPosition();
        FMOD_VECTOR fv = { pos.x, pos.y, pos.z };
        FMOD_VECTOR vel = { veh->m_vecMoveSpeed.x, veh->m_vecMoveSpeed.y, veh->m_vecMoveSpeed.z };

        // Delta natural entre start e target (pequeno nas marchas altas)
        float baseDelta = TARGET_PITCH - START_PITCH_PER_GEAR[gIndex];

        // Factor que cresce com a marcha (0 em gear=1, ~1 em gear=MAX_GEAR_INDEX)
        static int MAX_GEAR_INDEX = 5; // número de marchas usadas no fator
        float gearFactor = 0.0f;
        if (MAX_GEAR_INDEX > 1) gearFactor = float(gIndex - 1) / float(MAX_GEAR_INDEX - 1);

        // escala proporcional: nas marchas altas aplicamos mais ganho ao delta
        float gearScale = 1.0f + gearFactor * (PITCH_AMPLIFY_MAX - 1.0f);

        // gearMultiplier aumenta o sweep do pitch conforme a marcha cresce (tune aqui)
        float gearMultiplier = 1.0f + gearFactor * 1.1f; // 1.0..2.1 (ajuste se quiser mais)

        // (mantemos gearScale se quiser um leve aumento nas marchas altas)
        // alvo para modo acelerando: sweep relativo ao START_PITCH da marcha
        float accelTarget = START_PITCH_PER_GEAR[gIndex] + ratio * (TARGET_PITCH - START_PITCH_PER_GEAR[gIndex]) * gearScale;
        float upperLimit = TARGET_PITCH + MAX_OVERSHOOT;
        accelTarget = std::clamp(accelTarget, START_PITCH_PER_GEAR[gIndex], upperLimit);

        // alvo para modo desacelerando (baixo, mais notório)
        float decelTarget = START_PITCH_PER_GEAR[gIndex] + ratio * (TARGET_PITCH - START_PITCH_PER_GEAR[gIndex]) * gearScale * DECEL_FACTOR;
        decelTarget = std::clamp(decelTarget, START_PITCH_PER_GEAR[gIndex], accelTarget);
        decelTarget = std::max(decelTarget, MIN_PITCH);


        // decide modo baseado em input (isAccelerating já calculado antes)
        if (isAccelerating) {
            if (inst.engineMode != VehicleAudioInstance::EM_ACCEL) {
                inst.engineMode = VehicleAudioInstance::EM_ACCEL;
            }
            inst.desiredEnginePitch = accelTarget;
        }
        else {
            if (inst.engineMode != VehicleAudioInstance::EM_DECEL) {
                inst.engineMode = VehicleAudioInstance::EM_DECEL;
            }
            inst.desiredEnginePitch = decelTarget;
        }

        // smoothing: usa taxas diferentes para aceleração/desaceleração
        float dt = std::max(0.0f, CTimer::ms_fTimeStep);
        float baseAlpha = std::clamp(dt * PITCH_SMOOTHING, 0.0f, 1.0f);
        float alpha = baseAlpha;
        if (inst.engineMode == VehicleAudioInstance::EM_ACCEL) alpha = baseAlpha * ACCEL_SPEED_MULT;
        else if (inst.engineMode == VehicleAudioInstance::EM_DECEL) alpha = baseAlpha * DECEL_SPEED_MULT;

        // atualiza pitch com blend
        inst.currentPitch = inst.currentPitch + (inst.desiredEnginePitch - inst.currentPitch) * alpha;
        // segurança: não permitir pitch absurdo
        inst.currentPitch = std::clamp(inst.currentPitch, MIN_PITCH, TARGET_PITCH + MAX_OVERSHOOT);

        // --- shift drop decay (transient) ---
        float displayPitch = inst.currentPitch;
        if (inst.shiftPitchDrop != 0.0f) {
            unsigned int nowMs = CTimer::m_snTimeInMilliseconds;
            unsigned int elapsed = (nowMs > inst.shiftStartMs) ? (nowMs - inst.shiftStartMs) : 0;
            float t = std::min(1.0f, float(elapsed) / float(SHIFT_DROP_DURATION_MS));

            // decay suavizado (ease-out): usa (1 - t)^2 para fechamento mais natural
            float decayFactor = (1.0f - t);
            decayFactor = decayFactor * decayFactor; // acelera queda no fim
            displayPitch += inst.shiftPitchDrop * decayFactor;

            if (t >= 1.0f) {
                inst.shiftPitchDrop = 0.0f;
                inst.shiftStartMs = 0;
            }
        }

        try { inst.loopChannel->setPitch(displayPitch); }
        catch (...) {}


        // volume (mantém lógica anterior)
        float desiredVol = 0.45f + ratio * 0.55f;
        inst.currentVolume = inst.currentVolume + (desiredVol - inst.currentVolume) * baseAlpha;
        try { inst.loopChannel->setVolume(inst.currentVolume); }
        catch (...) {}


        // --- WIND loop control (novo: fade-in temporal + cap por-frame) ---
        if (inst.bank && inst.bank->sounds.count("wind")) {
            // calcula target a partir de velocidade/ratio e if accelerating
            float speedFactor = std::clamp(speed / WIND_SPEED_SCALE, 0.0f, 1.0f);
            float accelBoost = isAccelerating ? 1.0f : 0.6f;
            inst.targetWindVolume = WIND_MAX_VOL * speedFactor * accelBoost;

            // garante canal ativo (começa com volume 0) e regista início para fade-in
            if (!inst.windChannel) {
                FMOD::Sound* ws = inst.bank->sounds["wind"];
                if (ws) {
                    inst.windChannel = PlayLoop(veh, ws, 0.0f, 1.0f);
                    if (inst.windChannel) {
                        try { inst.windChannel->setVolume(0.0f); }
                        catch (...) {}
                        inst.windStartMs = CTimer::m_snTimeInMilliseconds;
                    }
                }
            }

            // se o canal já existia mas target subiu de 0 após despausar, garantimos fade-in também:
            if (inst.windChannel && inst.windStartMs == 0 && inst.currentWindVolume <= 0.0001f) {
                inst.windStartMs = CTimer::m_snTimeInMilliseconds;
            }

            // cap por-frame
            float dt2 = std::max(0.0f, CTimer::ms_fTimeStep);
            float maxDelta = MAX_WIND_RATE_PER_SEC * dt2; // ex: 0.25 * 0.016 = 0.004 por frame (~60fps)

            // compute fade progress (0..1) baseado no windStartMs
            float fadeFactor = 1.0f;
            if (inst.windStartMs != 0) {
                unsigned int nowMs = CTimer::m_snTimeInMilliseconds;
                unsigned int elapsed = (nowMs > inst.windStartMs) ? (nowMs - inst.windStartMs) : 0;
                fadeFactor = std::clamp(float(elapsed) / WIND_FADE_MS, 0.0f, 1.0f);
            }

            // desired volume considerando fade-in
            float desiredWithFade = inst.targetWindVolume * fadeFactor;

            // diferença e aplicação do cap por-frame (não ultrapassar maxDelta)
            float diff = desiredWithFade - inst.currentWindVolume;
            if (diff > maxDelta) diff = maxDelta;
            else if (diff < -maxDelta) diff = -maxDelta;
            inst.currentWindVolume += diff;

            // debug para log
            WriteLog("WIND: model=%d speed=%.2f speedF=%.3f target=%.3f fade=%.3f want=%.3f cur=%.3f accel=%d ch=%p",
                veh->m_nModelIndex, speed, speedFactor, inst.targetWindVolume, fadeFactor, desiredWithFade, inst.currentWindVolume,
                isAccelerating ? 1 : 0, (void*)inst.windChannel);

            // aplica volume e 3D attrs (se aplicável)
            if (inst.windChannel) {
                try { inst.windChannel->setVolume(inst.currentWindVolume); }
                catch (...) {}
                CVector pos2 = veh->GetPosition();
                FMOD_VECTOR fv2 = { pos2.x, pos2.y, pos2.z };
                FMOD_VECTOR vel2 = { veh->m_vecMoveSpeed.x, veh->m_vecMoveSpeed.y, veh->m_vecMoveSpeed.z };
                try { inst.windChannel->set3DAttributes(&fv2, &vel2); }
                catch (...) {}
            }

            // parar o canal se muito baixo e veículo praticamente parado
            if (inst.windChannel && inst.currentWindVolume < WIND_STOP_THRESHOLD && !isAccelerating && speed < 0.5f) {
                StopChannelSafe(inst.windChannel);
                inst.currentWindVolume = 0.0f;
                inst.targetWindVolume = 0.0f;
                inst.windStartMs = 0;
            }
        }




        try { inst.loopChannel->set3DAttributes(&fv, &vel); }
        catch (...) {}

        // restart if channel died (não tentar iniciar novo se jogo está pausado)
        bool isPlaying = true;
        if (inst.loopChannel && inst.loopChannel->isPlaying(&isPlaying) == FMOD_OK && !isPlaying) {
            WriteLog("Loop died; restarting loop for model=%d mode=%d", veh->m_nModelIndex, (int)inst.loopMode);
            std::string key = (inst.loopMode == LM_IDLE) ? "idle" : "engine";
            StopChannelSafe(inst.loopChannel);
            if (!IsGamePaused()) {
                FMOD::Sound* s = (inst.bank && inst.bank->sounds.count(key)) ? inst.bank->sounds[key] : nullptr;
                if (s) inst.loopChannel = PlayLoop(veh, s, inst.currentVolume, inst.currentPitch);
            }
        }
    }
}
// ---------------- Funções de pausa simplificada ----------------
static void SetPausedVolume(bool paused) {
    std::lock_guard<std::mutex> lk(g_mutex);

    for (auto& kv : g_vehicleInstances) {
        VehicleAudioInstance& inst = kv.second;

        try {
            // ---- LOOP CHANNEL ----
            if (inst.loopChannel) {
                if (paused) {
                    float vol = 0.0f;
                    if (inst.loopChannel->getVolume(&vol) == FMOD_OK) inst.storedLoopVolume = vol;
                    else inst.storedLoopVolume = inst.currentVolume;
                    inst.loopChannel->setVolume(0.0f);
                }
                else {
                    float restore = (inst.storedLoopVolume > 0.0f) ? inst.storedLoopVolume : inst.currentVolume;
                    inst.loopChannel->setVolume(restore);
                }
            }
            // ---- WIND CHANNEL ----
            if (inst.windChannel) {
                if (paused) {
                    float wv = 0.0f;
                    if (inst.windChannel->getVolume(&wv) == FMOD_OK) inst.storedWindVolume = wv;
                    else inst.storedWindVolume = inst.currentWindVolume;
                    try { inst.windChannel->setVolume(0.0f); }
                    catch (...) {}
                }
                else {
                    // não restaurar instantaneamente: usaremos fade-in controlado no UpdateInstance
                    float restore = (inst.storedWindVolume > 0.0f) ? inst.storedWindVolume : inst.currentWindVolume;
                    inst.targetWindVolume = restore;
                    inst.currentWindVolume = 0.0f;                      // parte de zero
                    inst.windStartMs = CTimer::m_snTimeInMilliseconds;  // força fade-in
                    try { inst.windChannel->setVolume(0.0f); }
                    catch (...) {}
                }
            }


            // ---- ATTACK / SHIFT (one-shots) ----
            if (inst.attackChannel) {
                if (paused) {
                    float av = 0.0f;
                    if (inst.attackChannel->getVolume(&av) == FMOD_OK) inst.storedAttackVolume = av;
                    else inst.storedAttackVolume = 1.0f;
                    inst.attackChannel->setVolume(0.0f);
                }
                else {
                    float restore = (inst.storedAttackVolume > 0.0f) ? inst.storedAttackVolume : 1.0f;
                    inst.attackChannel->setVolume(restore);
                }
            }
            if (inst.shiftChannel) {
                if (paused) {
                    // shiftChannel normalmente é também um one-shot; guardamos volume localmente no storedAttackVolume
                    float sv = 0.0f;
                    if (inst.shiftChannel->getVolume(&sv) == FMOD_OK) inst.storedAttackVolume = sv;
                    inst.shiftChannel->setVolume(0.0f);
                }
                else {
                    float restore = (inst.storedAttackVolume > 0.0f) ? inst.storedAttackVolume : 1.0f;
                    inst.shiftChannel->setVolume(restore);
                }
            }
        }
        catch (...) {
            // ignora erros de FMOD
        }
    }
}


// ---------------- main per-frame ----------------
static void OnProcess() {
    FMOD::System* core = GetCoreSystem();
    if (!core) return;

    // handle global pause/unpause transitions
    bool pausedNow = IsGamePaused();
    if (pausedNow && !g_gamePaused) {
        WriteLog("Game paused by user - muting volumes");
        SetPausedVolume(true);
        g_gamePaused = true;
    }
    else if (!pausedNow && g_gamePaused) {
        WriteLog("Game resumed - restoring volumes");
        SetPausedVolume(false);
        g_gamePaused = false;
    }

    // set listener from camera (não fatal)
    try {
        CVector camPos = TheCamera.GetPosition();
        CMatrix* camM = TheCamera.GetMatrix();
        FMOD_VECTOR lp = { camPos.x, camPos.y, camPos.z };
        FMOD_VECTOR lv = { 0.0f, 0.0f, 0.0f };
        FMOD_VECTOR lf = { camM->at.x, camM->at.y, camM->at.z };
        FMOD_VECTOR lu = { camM->up.x, camM->up.y, camM->up.z };
        core->set3DListenerAttributes(0, &lp, &lv, &lf, &lu);
    }
    catch (...) {}

    // iterar sobre instâncias — removemos APENAS quando ponteiro inválido
    std::vector<CVehicle*> toRemove;
    for (auto& kv : g_vehicleInstances) {
        CVehicle* v = kv.first;
        VehicleAudioInstance& inst = kv.second;

        // se ponteiro inválido -> parar canais e marcar para remoção
        if (!IsVehiclePointerValid(v)) {
            WriteLog("OnProcess: vehicle pointer invalid, stopping channels for model=%d", (v ? v->m_nModelIndex : -1));
            StopChannelSafe(inst.loopChannel);
            StopChannelSafe(inst.pendingLoopChannel);
            StopChannelSafe(inst.attackChannel);
            StopChannelSafe(inst.shiftChannel);
            StopChannelSafe(inst.windChannel); // parar wind também

            toRemove.push_back(v);
            continue;
        }

        // caso contrário, atualiza a instância normalmente (mesmo que o player esteja fora do carro)
        UpdateInstance(inst);
    }

    // efetua remoções depois do loop
    for (CVehicle* v : toRemove) {
        auto it = g_vehicleInstances.find(v);
        if (it != g_vehicleInstances.end()) {
            g_vehicleInstances.erase(it);
            WriteLog("OnProcess: removed audio instance for vehicle ptr=%p", (void*)v);
        }
    }

    // garante que existe instância para o veículo atual do player (se houver)
    CVehicle* playerVeh = FindPlayerVehicle(-1, true);
    if (playerVeh) {
        if (g_vehicleInstances.find(playerVeh) == g_vehicleInstances.end()) {
            VehicleAudioInstance inst;
            inst.vehicle = playerVeh;
            inst.loopChannel = nullptr;
            inst.pendingLoopChannel = nullptr;
            inst.attackChannel = nullptr;
            inst.shiftChannel = nullptr;
            inst.loopMode = LM_NONE;
            inst.bank = nullptr;
            inst.mutedGameAudio = false;
            inst.currentPitch = 1.0f;
            inst.currentVolume = 0.45f;
            inst.lastGear = INT_MIN;
            inst.wasAccelerating = false;
            inst.lastAccelReleaseMs = 0;
            inst.lastShiftUpMs = 0;
            inst.lastShiftDnMs = 0;
            inst.lastBackfireMs = 0;
            inst.windChannel = nullptr;
            inst.currentWindVolume = 0.0f;
            inst.targetWindVolume = 0.0f;
            inst.storedLoopVolume = 0.0f;
            inst.storedWindVolume = 0.0f;
            inst.storedAttackVolume = 0.0f;
            inst.attackChannel = nullptr;
            inst.shiftChannel = nullptr;
            g_vehicleInstances[playerVeh] = inst;
            WriteLog("Created audio instance for player vehicle modelId=%d", playerVeh->m_nModelIndex);
        }
    }

    // atualizar FMOD
    try {
        core->update();
    }
    catch (...) {}
}


// ---------------- init/shutdown FMOD ----------------
static void InitFMOD() {
    if (g_fmodCore) return;
    FMOD::System* system = nullptr;
    WriteLog("InitFMOD: starting...");
    FMOD_RESULT r = FMOD::System_Create(&system);
    if (r != FMOD_OK) { WriteLog("FMOD create failed r=%d", (int)r); return; }
    r = system->init(512, FMOD_INIT_NORMAL, nullptr);
    WriteLog("FMOD init result r=%d", (int)r);
    g_fmodCore = system;

    // tenta carregar logo.txd (não é fatal se não existir)
    LoadFMODLogo();

    // optional test
    std::string test = g_basePath + "\\test.wav";
    FMOD::Sound* ts = LoadWav(g_fmodCore, test, false);
    if (ts) { FMOD::Channel* ch = nullptr; g_fmodCore->playSound(ts, nullptr, false, &ch); WriteLog("Played test.wav"); }
}


static void ShutdownFMOD() {
    std::lock_guard<std::mutex> lk(g_mutex);
    if (g_fmodCore) { g_fmodCore->close(); g_fmodCore->release(); g_fmodCore = nullptr; }
    for (auto& kv : g_modelBanks) {
        if (!kv.second) continue;
        for (auto& p : kv.second->sounds) if (p.second) p.second->release();
        delete kv.second;
    }
    g_modelBanks.clear();
    for (auto& kv : g_vehicleInstances) {
        if (kv.second.loopChannel) kv.second.loopChannel->stop();
        if (kv.second.pendingLoopChannel) kv.second.pendingLoopChannel->stop();
        if (kv.second.attackChannel) kv.second.attackChannel->stop();
        if (kv.second.shiftChannel) kv.second.shiftChannel->stop();
        if (kv.second.windChannel) kv.second.windChannel->stop();
    }

    g_vehicleInstances.clear();
}

// ---------------- plugin ----------------
class VehicleSFXPlugin {
public:
    VehicleSFXPlugin() {
        InitLog();
        LoadConfig(PLUGIN_PATH((char*)"VehicleSFX.ini"));
        
        InitParams();
        Events::initGameEvent.after.Add([] { InitFMOD(); });

        // Process normal
        Events::processScriptsEvent += [] { OnProcess(); };

        // Quando o motor do jogo pede pra pausar todos os sons (ex.: ALT+TAB, menu etc)
        Events::onPauseAllSounds += []() {
            WriteLog("Events::onPauseAllSounds -> muting volumes");
            SetPausedVolume(true);
            };

        // O drawMenuBackgroundEvent é chamado enquanto o menu é desenhado
        Events::drawMenuBackgroundEvent += []() {
            try {
                DrawFMODLogoIfNeeded();
                if (FrontEndMenuManager.m_nCurrentMenuPage != eMenuPage::MENUPAGE_NONE) {
                    SetPausedVolume(true);
                }
                else {
                    SetPausedVolume(false);
                }
            }
            catch (...) {}
            };


        WriteLog("VehicleSFXPlugin constructed (menu/pause handlers attached)");
    }
    ~VehicleSFXPlugin() {
        ShutdownFMOD();
        WriteLog("VehicleSFXPlugin destructed");
    }
};
static VehicleSFXPlugin g_vehicleSFXPlugin;
