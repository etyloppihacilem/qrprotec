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

bool write_error_wav(const std::string &path) {
  const int                   rate    = 16000;
  const double                tones[] = { 880.0, 440.0 };
  std::vector< std::int16_t > samples;
  for (const double frequency : tones) {
    const int count = rate * 18 / 100;
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

Feedback::Feedback(Inateck &inateck) : inateck_(inateck) {
  std::error_code code;
  wav_path_ = (std::filesystem::temp_directory_path(code) / "qrprotec-erreur.wav").string();
  if (!write_error_wav(wav_path_))
    wav_path_.clear();
}

void Feedback::play_beep(const AppSettings &settings) {
  if (!settings.sound_enabled)
    return;
  std::string command = settings.beep_command;
  if (command.empty()) {
    if (wav_path_.empty()) {
      std::fputc('\a', stderr);
      return;
    }
    command = "aplay -q '%f' 2>/dev/null || paplay '%f' 2>/dev/null || printf '\\a'";
  }
  for (std::size_t position = command.find("%f"); position != std::string::npos; position = command.find("%f"))
    command.replace(position, 2, wav_path_);
  run_detached(command);
}

void Feedback::error(ScanSource source, const AppSettings &settings) {
  const bool from_sdk = source == ScanSource::Sdk;
  if (source == ScanSource::Phone && on_phone_error)
    on_phone_error(); // le telephone flashe et vibre, comme le bip de la douchette
  if (from_sdk && inateck_.sdk_connected())
    inateck_.signal_error(settings.scanner_signal);
  if (!from_sdk || !inateck_.sdk_connected())
    play_beep(settings);
  if (settings.flash_enabled && (!from_sdk || settings.flash_in_sdk_mode))
    flash_start_ = ImGui::GetTime();
}

void Feedback::test(const AppSettings &settings) {
  if (inateck_.sdk_connected())
    inateck_.signal_error(settings.scanner_signal);
  play_beep(settings);
  flash_start_ = ImGui::GetTime();
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
    ImGui::GetColorU32(ImVec4(0.9f, 0.05f, 0.05f, alpha))
  );
}

} // namespace qrprotec
