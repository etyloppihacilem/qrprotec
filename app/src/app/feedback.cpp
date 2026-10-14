/* ##################################646f75627420796f7572206f776e206578697374656e6365###################################

               """          feedback.cpp
        -\-    _|__
         |\___/  . \        Created on 29 Sep. 2026 at 16:00
         \     /(((/        by hmelica
          \___/)))/         hmelica@student.42.fr

##################################################################################################################### */

#include "feedback.hpp"

#include "../ui/inateck.hpp"
#include "imgui.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <vector>

extern char **environ;

namespace qrprotec {

namespace {

void write_u32(std::ofstream &output, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8)
    output.put(static_cast< char >((value >> shift) & 0xff));
}

void write_u16(std::ofstream &output, std::uint16_t value) {
  output.put(static_cast< char >(value & 0xff));
  output.put(static_cast< char >((value >> 8) & 0xff));
}

// Lance une commande shell sans bloquer l'interface.
void run_detached(const std::string &command) {
  std::thread([command]() {
    pid_t       pid     = 0;
    const char *argv[]  = { "/bin/sh", "-c", command.c_str(), nullptr };
    if (posix_spawn(&pid, "/bin/sh", nullptr, nullptr, const_cast< char *const * >(argv), environ) == 0) {
      int status = 0;
      waitpid(pid, &status, 0);
    }
  }).detach();
}

} // namespace

namespace {

bool write_tones_wav(const std::string &path, const std::vector< double > &tones, int duration_ms) {
  const int                   rate    = 16000;
  std::vector< std::int16_t > samples;
  for (const double frequency : tones) {
    const int count = rate * duration_ms / 1000;
    for (int index = 0; index < count; ++index) {
      const double time     = static_cast< double >(index) / rate;
      const double envelope = std::min(1.0, std::min(index, count - index) / (rate * 0.01));
      // onde carree adoucie : bien audible sur de petits haut-parleurs
      const double value = std::sin(2.0 * M_PI * frequency * time) >= 0.0 ? 0.6 : -0.6;
      samples.push_back(static_cast< std::int16_t >(value * envelope * 32767.0));
    }
    for (int index = 0; index < rate / 25; ++index)
      samples.push_back(0);
  }
  std::ofstream output(path, std::ios::binary);
  if (!output)
    return false;
  const std::uint32_t data_size = static_cast< std::uint32_t >(samples.size() * 2);
  output.write("RIFF", 4);
  write_u32(output, 36 + data_size);
  output.write("WAVEfmt ", 8);
  write_u32(output, 16);
  write_u16(output, 1);
  write_u16(output, 1);
  write_u32(output, rate);
  write_u32(output, rate * 2);
  write_u16(output, 2);
  write_u16(output, 16);
  output.write("data", 4);
  write_u32(output, data_size);
  output.write(reinterpret_cast< const char * >(samples.data()), static_cast< std::streamsize >(data_size));
  return static_cast< bool >(output);
}

} // namespace

bool write_error_wav(const std::string &path) {
  return write_tones_wav(path, { 880.0, 440.0 }, 180);
}

bool write_task_wav(const std::string &path) {
  return write_tones_wav(path, { 660.0, 880.0, 1100.0 }, 100);
}

bool write_notice_wav(const std::string &path) {
  return write_tones_wav(path, { 1000.0, 1000.0 }, 140);
}

Feedback::Feedback(Inateck &inateck) : inateck_(inateck) {
  std::error_code code;
  wav_path_ = (std::filesystem::temp_directory_path(code) / "qrprotec-erreur.wav").string();
  if (!write_error_wav(wav_path_))
    wav_path_.clear();
  task_wav_path_ = (std::filesystem::temp_directory_path(code) / "qrprotec-taches.wav").string();
  if (!write_task_wav(task_wav_path_))
    task_wav_path_.clear();
  notice_wav_path_ = (std::filesystem::temp_directory_path(code) / "qrprotec-signalement.wav").string();
  if (!write_notice_wav(notice_wav_path_))
    notice_wav_path_.clear();
}

void Feedback::play_beep(const AppSettings &settings, const std::string &wav) {
  if (!settings.sound_enabled)
    return;
  std::string command = settings.beep_command;
  if (command.empty()) {
    if (wav.empty()) {
      std::fputc('\a', stderr);
      return;
    }
    command = "aplay -q '%f' 2>/dev/null || paplay '%f' 2>/dev/null || afplay '%f' 2>/dev/null || printf '\\a'";
  }
  for (std::size_t position = command.find("%f"); position != std::string::npos; position = command.find("%f"))
    command.replace(position, 2, wav);
  run_detached(command);
}

void Feedback::error(ScanSource source, const AppSettings &settings) {
  const bool from_sdk = source == ScanSource::Sdk;
  if (source == ScanSource::Phone && on_phone_error)
    on_phone_error(); // le telephone flashe et vibre, comme le bip de la douchette
  if (from_sdk && inateck_.sdk_connected())
    inateck_.signal_error(settings.scanner_signal);
  if (!from_sdk || !inateck_.sdk_connected())
    play_beep(settings, wav_path_);
  if (settings.flash_enabled && (!from_sdk || settings.flash_in_sdk_mode)) {
    flash_start_ = ImGui::GetTime();
    flash_task_  = false;
  }
}

void Feedback::notice(ScanSource source, const AppSettings &settings) {
  if (source == ScanSource::Sdk && inateck_.sdk_connected())
    inateck_.signal_error(settings.scanner_signal);
  play_beep(settings, notice_wav_path_);
}

void Feedback::task(ScanSource source, const AppSettings &settings) {
  const bool from_sdk = source == ScanSource::Sdk;
  if (from_sdk && inateck_.sdk_connected()) {
    // LED bleue et bips courts : distinct du signal d'erreur (orange, bips longs)
    ScannerErrorSignal signal;
    signal.beep_on    = 1;
    signal.beep_off   = 1;
    signal.beep_count = 2;
    signal.led_color  = 3;
    signal.led_on     = 3;
    signal.led_off    = 2;
    signal.led_count  = 4;
    inateck_.signal_error(signal);
  }
  if (!from_sdk || !inateck_.sdk_connected())
    play_beep(settings, task_wav_path_);
  if (settings.flash_enabled && (!from_sdk || settings.flash_in_sdk_mode)) {
    flash_start_ = ImGui::GetTime();
    flash_task_  = true;
  }
}

void Feedback::test(const AppSettings &settings) {
  if (inateck_.sdk_connected())
    inateck_.signal_error(settings.scanner_signal);
  play_beep(settings, wav_path_);
  flash_start_ = ImGui::GetTime();
  flash_task_  = false;
}

void Feedback::draw_overlay() {
  if (flash_start_ < 0.0)
    return;
  const double elapsed  = ImGui::GetTime() - flash_start_;
  const double duration = 0.8; // deux clignotements
  if (elapsed > duration) {
    flash_start_ = -1.0;
    return;
  }
  const double phase = std::fmod(elapsed, 0.4) / 0.4;
  const float  alpha = phase < 0.5 ? 0.55f : 0.0f;
  if (alpha <= 0.0f)
    return;
  const ImGuiViewport *viewport = ImGui::GetMainViewport();
  ImGui::GetForegroundDrawList()->AddRectFilled(
    viewport->Pos,
    ImVec2(viewport->Pos.x + viewport->Size.x, viewport->Pos.y + viewport->Size.y),
    ImGui::GetColorU32(flash_task_ ? ImVec4(0.12f, 0.40f, 0.85f, alpha) : ImVec4(0.9f, 0.05f, 0.05f, alpha))
  );
}

} // namespace qrprotec
