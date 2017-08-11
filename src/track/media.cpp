/*
** Taiga
** Copyright (C) 2010-2017, Eren Okka
** 
** This program is free software: you can redistribute it and/or modify
** it under the terms of the GNU General Public License as published by
** the Free Software Foundation, either version 3 of the License, or
** (at your option) any later version.
** 
** This program is distributed in the hope that it will be useful,
** but WITHOUT ANY WARRANTY; without even the implied warranty of
** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
** GNU General Public License for more details.
** 
** You should have received a copy of the GNU General Public License
** along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <algorithm>

#include <windows/win/string.h>

#include "base/file.h"
#include "base/log.h"
#include "base/process.h"
#include "base/string.h"
#include "library/anime.h"
#include "library/anime_db.h"
#include "library/anime_episode.h"
#include "library/anime_util.h"
#include "taiga/path.h"
#include "taiga/settings.h"
#include "taiga/timer.h"
#include "track/media.h"
#include "track/recognition.h"
#include "ui/dlg/dlg_anime_info.h"
#include "ui/dialog.h"
#include "ui/ui.h"

class track::recognition::MediaPlayers MediaPlayers;

namespace track {
namespace recognition {

bool MediaPlayers::Load() {
  items.clear();

  std::vector<anisthesia::Player> players;
  const auto path = taiga::GetPath(taiga::Path::Media);

  std::wstring resource;
  win::ReadStringFromResource(L"IDR_PLAYERS", L"DATA", resource);
  if (anisthesia::ParsePlayersData(WstrToStr(resource), players)) {
    for (const auto& player : players) {
      items.push_back(player);
    }
  }

  players.clear();
  if (anisthesia::ParsePlayersFile(WstrToStr(path), players)) {
    for (const auto& player : players) {
      auto it = std::find_if(items.begin(), items.end(),
          [&player](const MediaPlayer& item) {
            return item.name == player.name;
          });
      if (it != items.end()) {
        LOGD(L"Override: " + StrToWstr(player.name));
        *it = player;
      } else {
        LOGD(L"Add: " + StrToWstr(player.name));
        items.push_back(player);
      }
    }
  }

  if (items.empty()) {
    ui::DisplayErrorMessage(L"Could not read media players data.",
                            path.c_str());
    return false;
  }

  return true;
}

////////////////////////////////////////////////////////////////////////////////

bool MediaPlayers::IsPlayerActive() const {
  if (!Settings.GetBool(taiga::kSync_Update_CheckPlayer))
    return true;

  return current_result_ &&
         current_result_->window.handle == GetForegroundWindow();
}

std::string MediaPlayers::current_player_name() const {
  if (current_result_)
    return current_result_->player.name;
  return std::string();
}

std::wstring MediaPlayers::current_title() const {
  return current_title_;
}

bool MediaPlayers::player_running() const {
  return player_running_;
}

void MediaPlayers::set_player_running(bool player_running) {
  player_running_ = player_running;
}

bool MediaPlayers::title_changed() const {
  return title_changed_;
}

void MediaPlayers::set_title_changed(bool title_changed) {
  title_changed_ = title_changed;
}

////////////////////////////////////////////////////////////////////////////////

std::vector<anisthesia::Player> GetEnabledPlayers(
    const std::vector<MediaPlayer>& players) {
  std::vector<anisthesia::Player> results;

  for (const auto& player : players) {
    if (!player.enabled)
      continue;

    switch (player.type) {
      default:
      case anisthesia::PlayerType::Default:
        if (!Settings.GetBool(taiga::kRecognition_DetectMediaPlayers))
          continue;
        break;
      case anisthesia::PlayerType::WebBrowser:
        if (!Settings.GetBool(taiga::kRecognition_DetectStreamingMedia))
          continue;
        break;
    }

    results.push_back(player);
  }

  return results;
}

bool VerifyMedia(const anisthesia::MediaInformation& media_information) {
  const auto value = StrToWstr(media_information.value);

  switch (media_information.type) {
    case anisthesia::MediaInformationType::File:
      if (!Meow.IsValidFileExtension(GetFileExtension(value)))
        return false;
      if (!Meow.IsValidAnimeType(value))
        return false;
      break;
  }

  return true;
}

bool GetTitleFromDefaultPlayer(const std::vector<anisthesia::Media>& media,
                               std::wstring& title) {
  for (const auto& item : media) {
    for (const auto& information : item.information) {
      auto value = StrToWstr(information.value);

      switch (information.type) {
        case anisthesia::MediaInformationType::File:
          TrimLeft(value, L"\\?");
          break;
      }

      title = value;
      return true;
    }
  }

  return false;
}

bool GetTitleFromWebBrowser(const std::vector<anisthesia::Media>& media,
                            const std::wstring& current_title,
                            std::wstring& current_page_title,
                            std::wstring& title) {
  std::wstring page_title;
  std::wstring url;
  std::vector<std::wstring> tabs;

  for (const auto& item : media) {
    for (const auto& information : item.information) {
      const auto value = StrToWstr(information.value);
      switch (information.type) {
        case anisthesia::MediaInformationType::Tab:
          tabs.push_back(value);
          break;
        case anisthesia::MediaInformationType::Title:
          page_title = value;
          break;
        case anisthesia::MediaInformationType::Url:
          url = value;
          break;
      }
    }
  }

  NormalizeWebBrowserTitle(url, page_title);
  for (auto& tab : tabs) {
    NormalizeWebBrowserTitle(url, tab);
  }

  if (anime::IsValidId(CurrentEpisode.anime_id)) {
    if (!page_title.empty() && page_title == current_page_title) {
      title = current_title;
      return true;
    }
    for (const auto& tab : tabs) {
      if (!tab.empty() && tab == current_page_title) {
        title = current_title;
        return true;
      }
    }
  }

  title = page_title;

  if (GetTitleFromStreamingMediaProvider(url, title)) {
    current_page_title = page_title;
    return true;
  } else {
    current_page_title.clear();
    return false;
  }
}

bool GetTitleFromResult(const anisthesia::win::Result& result,
                        const std::wstring& current_title,
                        std::wstring& current_page_title,
                        std::wstring& title) {
  switch (result.player.type) {
    case anisthesia::PlayerType::Default:
      return GetTitleFromDefaultPlayer(result.media, title);
    case anisthesia::PlayerType::WebBrowser:
      return GetTitleFromWebBrowser(result.media, current_title,
                                    current_page_title, title);
  }

  return false;
}

bool MediaPlayers::CheckRunningPlayers() {
  const auto enabled_players = GetEnabledPlayers(items);
  std::vector<anisthesia::win::Result> results;

  if (anisthesia::win::GetResults(enabled_players, VerifyMedia, results)) {
    // Stick with the previously detected window if possible
    if (current_result_ && anime::IsValidId(CurrentEpisode.anime_id)) {
      auto it = std::find_if(results.begin(), results.end(),
          [this](const anisthesia::win::Result& result) {
            return result.window.handle == current_result_->window.handle;
          });
      if (it != results.end())
        std::rotate(results.begin(), it, it + 1);  // Move to front
    }

    std::wstring title;

    for (const auto& result : results) {
      if (GetTitleFromResult(result, current_title_,
                             current_page_title_, title)) {
        current_result_.reset(new anisthesia::win::Result(result));

        if (current_title_ != title) {
          current_title_ = title;
          set_title_changed(true);
        }
        player_running_ = true;

        return true;
      }
    }
  }

  current_result_.reset();
  return false;
}

MediaPlayer* MediaPlayers::GetRunningPlayer() {
  if (current_result_)
    for (auto& item : items)
      if (item.name == current_result_->player.name)
        return &item;

  return nullptr;
}

}  // namespace recognition
}  // namespace track

////////////////////////////////////////////////////////////////////////////////

void ProcessMediaPlayerStatus(const MediaPlayer* media_player) {
  // Media player is running
  if (media_player) {
    ProcessMediaPlayerTitle(*media_player);

  // Media player is not running
  } else {
    auto anime_item = AnimeDatabase.FindItem(CurrentEpisode.anime_id, false);

    // Media player was running, and the media was recognized
    if (anime_item) {
      bool processed = CurrentEpisode.processed;  // TODO: temporary solution...
      CurrentEpisode.Set(anime::ID_UNKNOWN);
      EndWatching(*anime_item, CurrentEpisode);
      if (Settings.GetBool(taiga::kSync_Update_WaitPlayer)) {
        CurrentEpisode.anime_id = anime_item->GetId();
        CurrentEpisode.processed = processed;
        UpdateList(*anime_item, CurrentEpisode);
        CurrentEpisode.anime_id = anime::ID_UNKNOWN;
      }
      taiga::timers.timer(taiga::kTimerMedia)->Reset();

    // Media player was running, but the media was not recognized
    } else if (MediaPlayers.player_running()) {
      ui::ClearStatusText();
      CurrentEpisode.Set(anime::ID_UNKNOWN);
      MediaPlayers.set_player_running(false);
      ui::DlgNowPlaying.SetCurrentId(anime::ID_UNKNOWN);
      taiga::timers.timer(taiga::kTimerMedia)->Reset();
    }
  }
}

void ProcessMediaPlayerTitle(const MediaPlayer& media_player) {
  auto anime_item = AnimeDatabase.FindItem(CurrentEpisode.anime_id);

  if (CurrentEpisode.anime_id == anime::ID_UNKNOWN) {
    if (!Settings.GetBool(taiga::kApp_Option_EnableRecognition))
      return;

    // Examine title and compare it with list items
    bool ignore_file = false;
    static track::recognition::ParseOptions parse_options;
    parse_options.parse_path = true;
    parse_options.streaming_media = media_player.type == anisthesia::PlayerType::WebBrowser;
    if (Meow.Parse(MediaPlayers.current_title(), parse_options, CurrentEpisode)) {
      bool is_inside_library_folders = true;
      if (Settings.GetBool(taiga::kSync_Update_OutOfRoot))
        if (!CurrentEpisode.folder.empty() && !Settings.library_folders.empty())
          is_inside_library_folders = anime::IsInsideLibraryFolders(CurrentEpisode.folder);
      if (is_inside_library_folders) {
        static track::recognition::MatchOptions match_options;
        match_options.allow_sequels = true;
        match_options.check_airing_date = true;
        match_options.check_anime_type = true;
        match_options.check_episode_number = true;
        auto anime_id = Meow.Identify(CurrentEpisode, true, match_options);
        if (anime::IsValidId(anime_id)) {
          // Recognized
          anime_item = AnimeDatabase.FindItem(anime_id);
          MediaPlayers.set_title_changed(false);
          CurrentEpisode.Set(anime_item->GetId());
          StartWatching(*anime_item, CurrentEpisode);
          return;
        } else if (!Meow.IsValidAnimeType(CurrentEpisode)) {
          ignore_file = true;
        } else if (!CurrentEpisode.file_extension().empty() &&
                   !Meow.IsValidFileExtension(CurrentEpisode.file_extension())) {
          ignore_file = true;
        }
      } else {
        ignore_file = true;
      }
    }
    // Not recognized
    CurrentEpisode.Set(anime::ID_NOTINLIST);
    if (!ignore_file)
      ui::OnRecognitionFail();

  } else {
    if (MediaPlayers.title_changed()) {
      // Caption changed
      MediaPlayers.set_title_changed(false);
      ui::ClearStatusText();
      bool processed = CurrentEpisode.processed;  // TODO: not a good solution...
      CurrentEpisode.Set(anime::ID_UNKNOWN);
      if (anime_item) {
        EndWatching(*anime_item, CurrentEpisode);
        CurrentEpisode.anime_id = anime_item->GetId();
        CurrentEpisode.processed = processed;
        UpdateList(*anime_item, CurrentEpisode);
        CurrentEpisode.anime_id = anime::ID_UNKNOWN;
      } else {
        ui::DlgNowPlaying.SetCurrentId(anime::ID_UNKNOWN);
      }
      taiga::timers.timer(taiga::kTimerMedia)->Reset();
    }
  }
}
