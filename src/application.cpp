#include "fuzzy.hpp"
#include "history.hpp"
#include "tmux.hpp"
#include "catalog.hpp"
#include "util.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <ftxui.hpp>
#include <string_view>
#include <thread>

using namespace ftxui;

struct Match
{
  size_t index;                 // Position in entries
  int score;                    // Fuzzy score + hist bonus
  std::vector<size_t> indices;  // Highlighted chars position
};

static constexpr std::array entry_styles{
  std::pair{std::string_view{" SSH "}, Color::Cyan},
  std::pair{std::string_view{" DOC "}, Color::Blue},
  std::pair{std::string_view{" MUX "}, Color::Green},
};

static Color
classify_text_color(std::string_view text)
{
  if      (text == "running") { return Color::Green; }
  else if (text == "stopped") { return Color::Red; }
  else                        { return Color::Blue; }
}

static Color
selection_color()
{
  return is_dark_theme() ? Color::Grey30 : Color::RGB(232, 232, 232);
}

static ftxui::Element
label(Entry entry)
{
  auto& [label, label_color] = entry_styles[static_cast<size_t>(entry.kind())];
  return text(std::string{label}) | color(Color::RGB(0, 0, 0)) | bgcolor(label_color) | bold;
}

/* I don't think i like the dimming implemetnation */
[[maybe_unused]] static Element
highlighted(std::string_view text, std::span<const size_t> indices, size_t dim_until)
{
  Elements parts;
  dim_until = 0;
  for (size_t pos = 0; pos < text.size();)
  {
    bool matched = std::ranges::binary_search(indices, pos);
    bool dimmed = pos < dim_until;
    size_t end = pos + 1;
    while (end < text.size() && std::ranges::binary_search(indices, end) == matched && (end < dim_until) == dimmed)
    {
      ++end;
    }
    
    auto part = ftxui::text(std::string{text.substr(pos, end - pos)});
    if (matched) { part = part | bgcolor(Color::Yellow) | color(Color::Red) | bold; }
    else if (dimmed) { part = part | dim; }
    parts.push_back(std::move(part));
    pos = end;
  }
  return hbox(std::move(parts));
}

static Element
highlighted(std::string_view text, std::span<const size_t> indices)
{
  Elements parts;
  for (size_t pos = 0; pos < text.size();)
  {
    bool matched = std::ranges::binary_search(indices, pos);
    size_t end = pos + 1;
    while (end < text.size() && std::ranges::binary_search(indices, end) == matched) { ++end; }
    
    auto part = ftxui::text(std::string{text.substr(pos, end - pos)});
    auto bg_color = is_dark_theme() ? bgcolor(Color::RGB(242, 208, 107)): bgcolor(Color::RGB(234, 183, 0));
    if (matched) { part = part | bg_color | color(Color::Red) | bold; }
    parts.push_back(std::move(part));
    pos = end;
  }
  return hbox(std::move(parts));
}

static void
delete_word(std::string& text)
{
  constexpr std::string_view whitespace{" \t\n\r\f\v"};
  text.erase(text.find_last_not_of(whitespace) + 1);
  text.erase(text.find_last_of(whitespace) + 1);
}

std::vector<Match>
matches(Entries& entries, std::string_view query, const HistoryEntries& hentries)
{
  std::vector<Match> result;
  for (size_t i = 0; i < entries.size(); ++i)
  {
    std::string_view text = entries[i].display();
    int64_t hscore = history_score(hentries, entries[i].info());
    if (trim(query).empty())
    { /* empty query gives all entries score of 1 */
      result.emplace_back(i, 1 + static_cast<int64_t>(hscore * 0.5), std::vector<size_t>{});
      continue;
    }
    auto fuzzy = fuzzy_match(text, query, entries[i].kind());
    if (!fuzzy.matched) { continue; }

    int64_t score = fuzzy.score + static_cast<int64_t>(fuzzy.score * hscore / 60);
    result.emplace_back(i, score, std::move(fuzzy.indices));
  }

  // priority sorting
  std::ranges::sort(result, [&](Match& left, Match& right){
      auto& a = entries[left.index];
      auto& b = entries[right.index];
      if (left.score != right.score)  { return left.score > right.score; }
      if (a.active() != b.active())   { return a.active() > b.active(); }
      if (a.kind() != b.kind())       { return a.kind() > b.kind(); }
      return left.index > right.index;
  });

  return result;
}

void
merge_entries(Entries& entries, Entries& incoming_entries)
{
  /* Skip the tmux entries that are active and created my another entry type
   * so we don't have duplicated entryes of <other_type> and Tmux */
  for (const auto& incoming : incoming_entries)
  {
    if (incoming.kind() == EntryKind::TmuxEntry)
    { /* skip the tmux entry if entries with the same name exists */
      auto represented = std::ranges::any_of(entries,[&](const Entry& current){
          return current.kind() != EntryKind::TmuxEntry && incoming.key() == current.key();
      });
      if (represented) { continue; }
    }
    else
    { /* remove the tmux entry that has the same name with the soon to be merged entry */
      std::erase_if(entries,[&](const Entry& current){
          return current.kind() == EntryKind::TmuxEntry && current.key() == incoming.key();
      });
    }
    entries.push_back(incoming);
  }
}

/* define the work of each worker */
template<typename Loader>
static std::jthread
launch_loader(Entries& current_entries, ScreenInteractive& screen, int& loading, Loader loader)
{
  return std::jthread([&screen, loader = std::move(loader), &current_entries, &loading]() mutable {
    auto entries = loader();
    screen.Post([entries = std::move(entries), &current_entries, &loading]() mutable {
        merge_entries(current_entries, entries);
        --loading;
    });
    screen.PostEvent(Event::Custom);
  });
}

/* run the ui */
int
run_ui()
{
  Entries entries;
  auto app = ftxui::App::Fullscreen();
  const auto active_entries = active_tmux_sessions();
  int loading = static_cast<int>(EntryKind::Count);
  const std::array loaders{
    launch_loader(entries, app, loading, [&]{ return ssh_entries(active_entries); }),
    launch_loader(entries, app, loading, [&]{ return docker_entries(active_entries); }),
    launch_loader(entries, app, loading, [&]{ return tmux_entries(active_entries); }),
  };
  HistoryEntries history_entries = load_history();
  std::string s_query{};  // the actual query written
  std::string s_status{}; // status to show on operations
  size_t s_selected{0};   // the selected entry index
  Color selected_color = selection_color();

  /* Renderer Implementation */
  auto renderer = Renderer([&](){
    const auto filtered = matches(entries, s_query, history_entries);
    Elements rows;
    for (size_t pos = 0; pos < filtered.size(); ++pos)
    {
      const auto& match = filtered[pos];
      const auto& entry = entries[match.index];
      const bool selected =  pos == s_selected;
      const auto style = selected ? bgcolor(selected_color) | bold | focus : nothing;

      /* row construction */
      Elements row{ label(entry), text(" "), highlighted(entry.display(), match.indices)};
      if (entry.active()) { row.push_back(text("*") | color(Color::Green)); }
      row.push_back(text("  "));
      row.push_back(text(entry.extra) | color(classify_text_color(entry.extra)));
      row.push_back(filler());
      row.push_back(text("<" +std::to_string(match.score)+ ">") | color(Color::GrayDark));
      rows.push_back(hbox(std::move(row)) | style);
      /* end row construction */
    }
    if (loading != 0) { rows.push_back(text("Loading Entries") | dim); }
    if (rows.empty())   { rows.push_back(text("No matching entries") | dim); }

    Elements search{text("> ") | bold, text(s_query), text(" ") | focusCursorBlock};
    if (!s_status.empty())
    {
      search.push_back(text("  " + s_status) | color(Color::Yellow));
    }
    return vbox({hbox(std::move(search)), separator(),
        vbox(std::move(rows)) | yframe | flex}) | border;
  });

  /* Keybinds logic */
  auto component = CatchEvent(renderer, [&](Event event) {
    const auto quit = [&] {
      app.ExitLoopClosure()();
      return true;
    };
    if (event == Event::Escape || event == Event::CtrlC || (event == Event::Character("q") && s_query.empty()))
    {
      return quit();
    }
    if (event == Event::Backspace || event == Event::CtrlU ||
        event == Event::CtrlW || event.is_character())
    {
      if (event == Event::Backspace && !s_query.empty()) { s_query.pop_back(); }
      else if (event == Event::CtrlU) { s_query.clear(); }
      else if (event == Event::CtrlW) { delete_word(s_query); }
      else if (event.is_character()) { s_query += event.character(); }
      s_selected = 0;
      s_status.clear();
      return true;
    }

    const auto filtered = matches(entries, s_query, history_entries);
    if (s_selected >= filtered.size())
    {
      s_selected = filtered.empty() ? 0 : filtered.size() - 1;
    }
    const Entry* selected_entry = filtered.empty() ? nullptr
                                                   : &entries[filtered[s_selected].index];
    if (event == Event::Return)
    {
      if (selected_entry == nullptr) { return true; }
      record_history(history_entries, selected_entry->info());
      execute(*selected_entry, active_entries);
      return quit();
    }
    if (event == Event::ArrowUp || event == Event::CtrlK)
    {
      if (s_selected > 0) --s_selected;
      return true;
    }
    if (event == Event::ArrowDown || event == Event::CtrlJ)
    {
      if (s_selected + 1 < filtered.size()) ++s_selected;
      return true;
    }
    if (event == Event::PageUp)
    {
      s_selected = s_selected > 5 ? s_selected - 5 : 0;
      return true;
    }
    if (event == Event::PageDown)
    {
      s_selected = std::min(s_selected + 5, filtered.empty() ? 0 : filtered.size() - 1);
      return true;
    }
    if (event == Event::CtrlY)
    {
      if (selected_entry != nullptr && copy_info(selected_entry->info()))
      {
          s_status = "Copied: " + std::string{selected_entry->info()};
      }
      return true;
    }
    if (event == Event::CtrlT)
    {
      auto theme = toggle_theme();
      selected_color = selection_color();
      s_status = "Swithced to " + theme;
      return true;
    }
    return false;
  });

  app.Loop(component);
  return 0;
}

enum class PaneAction { Window, SplitHorizontal, SplitVertical };
/* Create a new tmux window or split based on the active pane state. */
bool
smart_new_tmux_pane(std::string pane_cmd, std::string pane_pid, std::string_view pane_current_path, PaneAction action)
{
  std::vector<std::string> args;
  if (pane_cmd == "ssh" || pane_cmd == "docker")
  { /* Get the running process's command arguments. */
    args = pane_process_args(pane_pid, pane_cmd);
  }

  /* Only repeat interactive Docker shells, never docker run or arbitrary commands. */
  if (pane_cmd == "docker" &&
      !(args.size() == 5 && args[1] == "exec" &&
        (args[2] == "-it" || args[2] == "-ti") &&
        (args[4] == "bash" || args[4] == "sh" || args[4] == "zsh" || args[4] == "fish")))
  {
    args.clear();
  }
  std::vector<std::string_view> cmd{"tmux"};
  switch (action)
  {
    case PaneAction::Window: cmd.push_back("new-window"); break;
    case PaneAction::SplitHorizontal: cmd.insert(cmd.end(), {"split-window", "-h"}); break;
    case PaneAction::SplitVertical: cmd.insert(cmd.end(), {"split-window", "-v"}); break;
  }
  cmd.insert(cmd.end(), {"-c", pane_current_path});
  for (const auto& arg : args) { cmd.push_back(arg); }
  return run_command(cmd).exit_code == 0;
}

std::string
short_pwd(std::string_view pwd)
{
  std::string result{};
  auto home = home_dir().string();
  if (!home.empty() && pwd.starts_with(home) &&
      (pwd.size() == home.size() || pwd[home.size()] == '/'))
  { /* replace $HOME/ with ~/ */
    result += "~/";
    pwd.remove_prefix(home.size());
    if (pwd.starts_with('/')) { pwd.remove_prefix(1); }
  }
  else if (pwd.starts_with('/'))
  {
    result += '/';
    pwd.remove_prefix(1);
  }

  while (!pwd.empty())
  {
    if (auto end = pwd.find('/'); end != std::string_view::npos)
    {
      auto dir = pwd.substr(0, end);
      if (!dir.empty())
      {
        /* show 2 chars if the directory is hidden */
        if (dir[0] == '.')  { result += dir.substr(0, 2); }
        else                { result += dir[0]; }
      }
      pwd.remove_prefix(end + 1);
      result += '/';
    }
    else
    {
      result += pwd;
      break;
    }
  }

  return result;
}

int
main(int argc, char* argv[])
{
  if (argc == 5)
  {
    const std::string_view cmd{argv[1]};
    if (cmd == "new-pane")
    {
      return smart_new_tmux_pane(argv[2], argv[3], argv[4], PaneAction::Window) ? 0 : 1;
    }
    if (cmd == "split-horizontal")
    {
      return smart_new_tmux_pane(argv[2], argv[3], argv[4], PaneAction::SplitHorizontal) ? 0 : 1;
    }
    if (cmd == "split-vertical")
    {
      return smart_new_tmux_pane(argv[2], argv[3], argv[4], PaneAction::SplitVertical) ? 0 : 1;
    }
  }
  if (argc == 3 && std::string_view{argv[1]} == "path") 
  {
    std::puts(short_pwd(argv[2]).c_str());
    return 0;
  }
  if (argc != 1)
  {
    std::fprintf(stderr,
                 "Usage: allmux\n"
                 "       allmux path PATH\n"
                 "       allmux new-pane PANE_COMMAND PANE_PID PANE_PATH\n"
                 "       allmux split-horizontal PANE_COMMAND PANE_PID PANE_PATH\n"
                 "       allmux split-vertical PANE_COMMAND PANE_PID PANE_PATH\n");
    return 1;
  }
  return run_ui();
}
