/**
 * @file        filesystem/first_launch.cpp
 * @brief       First start from a disc image (see first_launch.h)
 *
 * The idea and the flow are haloce-nx's (CC0), which unpacks its Xbox disc the same way on first start. First
 * written for Halo 3, moved here so every title uses one copy.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#include <rex/filesystem/first_launch.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <system_error>
#include <vector>

#include <rex/filesystem/disc_image_extract.h>

#if defined(__SWITCH__)
#include <switch.h>
#endif

namespace rex::filesystem::first_launch {
namespace {

std::string g_summary;

std::filesystem::path FindImage(const std::filesystem::path& folder) {
  std::error_code ec;
  std::vector<std::filesystem::path> images;
  for (const auto& e : std::filesystem::directory_iterator(folder, ec)) {
    std::string ext = e.path().extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    if ((ext == ".iso" || ext == ".xiso") && e.is_regular_file(ec)) {
      images.push_back(e.path());
    }
  }
  return images.empty() ? std::filesystem::path{} : *std::min_element(images.begin(), images.end());
}

// The screen while copying: libnx's console on the Switch, the terminal elsewhere.
class Screen {
 public:
  Screen() {
#if defined(__SWITCH__)
    consoleInit(nullptr);
    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    padInitializeDefault(&pad_);
#endif
  }
  ~Screen() {
#if defined(__SWITCH__)
    consoleExit(nullptr);
#endif
  }
  void Print(const char* text) {
    std::fputs(text, stdout);
    std::fflush(stdout);
#if defined(__SWITCH__)
    consoleUpdate(nullptr);
#endif
  }
  // Leaves a failure on screen until a button is pressed, instead of the program closing on a black screen.
  void WaitForButton() {
#if defined(__SWITCH__)
    Print("\n\nPress A to close.\n");
    while (appletMainLoop()) {
      padUpdate(&pad_);
      if (padGetButtonsDown(&pad_) & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)) {
        break;
      }
      svcSleepThread(16'666'667);
    }
#endif
  }

 private:
#if defined(__SWITCH__)
  PadState pad_{};
#endif
};

}  // namespace

const std::string& Summary() { return g_summary; }

std::filesystem::path ExtractIfNeeded(const std::filesystem::path& folder, const Options& options) {
  const auto game = folder / std::string(options.folder);
  std::error_code ec;
  if (std::filesystem::is_regular_file(game / "default.xex", ec) && !DiscExtractionUnfinished(game)) {
    return {};  // already there
  }
  const auto image = FindImage(folder);
  if (image.empty()) {
    return {};
  }
  const bool resuming = DiscExtractionUnfinished(game);

  Screen screen;
  char line[512];
  std::snprintf(line, sizeof(line),
                "\x1b[2J\x1b[1;1H%.*s: UNPACKING THE GAME\n\n"
                "%s the game out of %s into %.*s/.\nThis only happens once.\n\n",
                int(options.title.size()), options.title.data(), resuming ? "Carrying on copying" : "Copying",
                image.filename().string().c_str(), int(options.folder.size()), options.folder.data());
  screen.Print(line);

  const auto started = std::chrono::steady_clock::now();
  DiscExtractOptions extract;
  extract.skip = options.skip;
  extract.progress = [&](const DiscExtractProgress& p) {
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    const double rate = seconds > 1 ? double(p.done) / seconds : 0;  // includes skipped files on a resume
    const double left = rate > 0 ? double(p.total - p.done) / rate : 0;
    std::snprintf(line, sizeof(line), "\x1b[7;1H%-60.60s\n%6.2f of %.2f GB  (%3d %%)   %s%-12s\n",
                  std::string(p.file).c_str(), double(p.done) / 1e9, double(p.total) / 1e9,
                  p.total ? int(p.done * 100 / p.total) : 0, rate > 0 ? "about " : "",
                  rate > 0 ? (std::to_string(int(left / 60) + 1) + " min left").c_str() : "");
    screen.Print(line);
    return true;
  };
  std::string error;
  if (!ExtractDiscImage(image, game, extract, &error)) {
    g_summary = "could not unpack " + image.filename().string() + ": " + error;
    std::snprintf(line, sizeof(line), "\n\nThe game could not be copied:\n%s\n", error.c_str());
    screen.Print(line);
    screen.WaitForButton();
    return {};
  }
  const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
  g_summary = "unpacked " + image.filename().string() + " into " + game.string() + " in " +
              std::to_string(int(seconds)) + " s";
  return game;
}

}  // namespace rex::filesystem::first_launch
