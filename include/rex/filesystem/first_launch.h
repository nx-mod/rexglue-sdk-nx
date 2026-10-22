/**
 * @file        filesystem/first_launch.h
 * @brief       First start from a disc image: unpacks an .iso placed next to the program
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 */

#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>

namespace rex::filesystem::first_launch {

struct Options {
  // Shown at the top of the screen while copying, e.g. "HALO 3" or "NEED FOR SPEED: MOST WANTED".
  std::string_view title;
  // The folder the disc is copied into, next to the image ("game", "game_root").
  std::string_view folder = "game";
  // Top-level disc entries to leave out, compared without case (e.g. "$SystemUpdate").
  std::span<const std::string_view> skip;
};

// For players: put the program and the game's .iso (or .xiso) in the same folder. When `folder` has no complete game
// in `folder/options.folder` but holds an image, the first start copies the disc there, with its progress on screen,
// and returns that folder; every start after that finds the copy and returns an empty path (the caller then picks
// the folder as usual). On the Switch the image cannot be read in place: the disc reader maps the whole file, and
// 7 GB does not fit.
//
// The copy can be interrupted: `<folder>/.extracting` stays until it finishes and the next start carries on,
// skipping the files already copied. The image is never deleted; once the copy is complete it can be removed.
//
// Progress goes to libnx's text console on the Switch (the terminal elsewhere). Call it before the window opens
// (ReXApp::OnConfigurePaths): the console has the screen to itself and is closed before the window takes it.
// A failure stays on screen until a button is pressed, then an empty path is returned (reason in Summary()).
std::filesystem::path ExtractIfNeeded(const std::filesystem::path& folder, const Options& options);

// What happened, for the log (logging starts after the paths are set; see ReXApp::OnPostInitLogging).
const std::string& Summary();

}  // namespace rex::filesystem::first_launch
