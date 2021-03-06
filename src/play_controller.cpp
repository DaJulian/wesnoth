/*
   Copyright (C) 2006 - 2015 by Joerg Hinrichs <joerg.hinrichs@alice-dsl.de>
   wesnoth playlevel Copyright (C) 2003 by David White <dave@whitevine.net>
   Part of the Battle for Wesnoth Project http://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
*/

/**
 *  @file
 *  Handle input via mouse & keyboard, events, schedule commands.
 */

#include "play_controller.hpp"

#include "actions/create.hpp"
#include "actions/heal.hpp"
#include "actions/undo.hpp"
#include "actions/vision.hpp"
#include "ai/manager.hpp"
#include "ai/testing.hpp"
#include "dialogs.hpp"
#include "display_chat_manager.hpp"
#include "formula_string_utils.hpp"
#include "game_events/manager.hpp"
#include "game_events/menu_item.hpp"
#include "game_events/pump.hpp"
#include "game_preferences.hpp"
#include "game_state.hpp"
#include "hotkey/hotkey_item.hpp"
#include "hotkey_handler.hpp"
#include "map_label.hpp"
#include "gettext.hpp"
#include "gui/dialogs/transient_message.hpp"
#include "halo.hpp"
#include "hotkey/command_executor.hpp"
#include "loadscreen.hpp"
#include "log.hpp"
#include "pathfind/teleport.hpp"
#include "preferences_display.hpp"
#include "replay.hpp"
#include "reports.hpp"
#include "resources.hpp"
#include "savegame.hpp"
#include "saved_game.hpp"
#include "save_blocker.hpp"
#include "scripting/game_lua_kernel.hpp"
#include "scripting/plugins/context.hpp"
#include "sound.hpp"
#include "soundsource.hpp"
#include "statistics.hpp"
#include "synced_context.hpp"
#include "terrain_type_data.hpp"
#include "tooltips.hpp"
#include "unit.hpp"
#include "unit_id.hpp"
#include "unit_types.hpp"
#include "whiteboard/manager.hpp"
#include "wml_exception.hpp"

#include <boost/foreach.hpp>
#include <boost/make_shared.hpp>
#include <boost/assign/list_of.hpp>

static lg::log_domain log_aitesting("aitesting");
#define LOG_AIT LOG_STREAM(info, log_aitesting)
//If necessary, this define can be replaced with `#define LOG_AIT std::cout` to restore previous behavior

static lg::log_domain log_engine("engine");
#define LOG_NG LOG_STREAM(info, log_engine)
#define DBG_NG LOG_STREAM(debug, log_engine)

static lg::log_domain log_display("display");
#define ERR_DP LOG_STREAM(err, log_display)

static lg::log_domain log_enginerefac("enginerefac");
#define LOG_RG LOG_STREAM(info, log_enginerefac)

static lg::log_domain log_engine_enemies("engine/enemies");
#define DBG_EE LOG_STREAM(debug, log_engine_enemies)

/**
 * Copies [scenario] attributes/tags that are not otherwise stored in C++ structs/clases.
 */
static void copy_persistent(const config& src, config& dst)
{
	typedef boost::container::flat_set<std::string> stringset;

	static stringset attrs = boost::assign::list_of
			("id")
			("theme")
			("next_scenario")
			("description")
			("name")
			("defeat_music")
			("victory_music")
			("victory_when_enemies_defeated")
			("remove_from_carryover_on_defeat")
			("disallow_recall")
			("experience_modifier")
			("require_scenario")
		.convert_to_container<stringset>();

	static stringset tags = boost::assign::list_of
			("terrain_graphics")
			("lua")
		.convert_to_container<stringset>();

	BOOST_FOREACH(const std::string& attr, attrs)
	{
		dst[attr] = src[attr];
	}

	BOOST_FOREACH(const std::string& tag, tags)
	{
		dst.append_children(src, tag);
	}
}

static void clear_resources()
{
	resources::controller = NULL;
	resources::filter_con = NULL;
	resources::gameboard = NULL;
	resources::gamedata = NULL;
	resources::game_events = NULL;
	resources::lua_kernel = NULL;
	resources::persist = NULL;
	resources::screen = NULL;
	resources::soundsources = NULL;
	resources::teams = NULL;
	resources::tod_manager = NULL;
	resources::tunnels = NULL;
	resources::undo_stack = NULL;
	resources::recorder = NULL;
	resources::units = NULL;
	resources::whiteboard.reset();
	resources::classification = NULL;
	resources::mp_settings = NULL;
}

play_controller::play_controller(const config& level, saved_game& state_of_game,
		const config& game_config, const tdata_cache& tdata,
		CVideo& video, bool skip_replay)
	: controller_base(game_config, video)
	, observer()
	, savegame_config()
	, ticks_(SDL_GetTicks())
	, tdata_(tdata)
	, gamestate_()
	, level_()
	, saved_game_(state_of_game)
	, prefs_disp_manager_()
	, tooltips_manager_()
	, whiteboard_manager_()
	, plugins_context_()
	, labels_manager_()
	, help_manager_(&game_config)
	, mouse_handler_(NULL, *this)
	, menu_handler_(NULL, *this, game_config)
	, hotkey_handler_(new hotkey_handler(*this, saved_game_))
	, soundsources_manager_()
	, persist_()
	, gui_()
	, xp_mod_(new unit_experience_accelerator(level["experience_modifier"].to_int(100)))
	, statistics_context_(new statistics::scenario_context(level["name"]))
	, replay_(new replay(state_of_game.get_replay()))
	, skip_replay_(skip_replay)
	, linger_(false)
	, init_side_done_now_(false)
	, victory_when_enemies_defeated_(level["victory_when_enemies_defeated"].to_bool(true))
	, remove_from_carryover_on_defeat_(level["remove_from_carryover_on_defeat"].to_bool(true))
	, victory_music_()
	, defeat_music_()
	, scope_()
	, player_type_changed_(false)
{
	copy_persistent(level, level_);
	
	resources::controller = this;
	resources::persist = &persist_;
	resources::recorder = replay_.get();

	resources::classification = &saved_game_.classification();
	resources::mp_settings = &saved_game_.mp_settings();

	persist_.start_transaction();

	// Setup victory and defeat music
	set_victory_music_list(level_["victory_music"]);
	set_defeat_music_list(level_["defeat_music"]);

	game_config::add_color_info(level);
	hotkey::deactivate_all_scopes();
	hotkey::set_scope_active(hotkey::SCOPE_GAME);
	try {
		init(video, level);
	} catch (...) {
		clear_resources();
		throw;
	}
}

play_controller::~play_controller()
{
	hotkey::delete_all_wml_hotkeys();
	clear_resources();
}

struct throw_end_level
{
	void operator()(const config&)
	{
		throw_quit_game_exception();
	}
};

void play_controller::init(CVideo& video, const config& level)
{
	util::scoped_resource<loadscreen::global_loadscreen_manager*, util::delete_item> scoped_loadscreen_manager;
	loadscreen::global_loadscreen_manager* loadscreen_manager = loadscreen::global_loadscreen_manager::get();
	if (!loadscreen_manager)
	{
		scoped_loadscreen_manager.assign(new loadscreen::global_loadscreen_manager(video));
		loadscreen_manager = scoped_loadscreen_manager.get();
	}

	loadscreen::start_stage("load level");

	LOG_NG << "initializing game_state..." << (SDL_GetTicks() - ticks()) << std::endl;
	gamestate_.reset(new game_state(level, *this, tdata_));
	
	resources::gameboard = &gamestate().board_;
	resources::gamedata = &gamestate().gamedata_;
	resources::teams = &gamestate().board_.teams_;
	resources::tod_manager = &gamestate().tod_manager_;
	resources::units = &gamestate().board_.units_;
	resources::filter_con = &gamestate();
	resources::undo_stack = &undo_stack();
	
	gamestate_->init(level, *this);
	resources::tunnels = gamestate().pathfind_manager_.get();

	LOG_NG << "initializing whiteboard..." << (SDL_GetTicks() - ticks()) << std::endl;
	whiteboard_manager_.reset(new wb::manager());
	resources::whiteboard = whiteboard_manager_;

	LOG_NG << "loading units..." << (SDL_GetTicks() - ticks()) << std::endl;
	loadscreen::start_stage("load units");
	preferences::encounter_all_content(gamestate().board_);

	LOG_NG << "initializing theme... " << (SDL_GetTicks() - ticks()) << std::endl;
	loadscreen::start_stage("init theme");
	const config& theme_cfg = controller_base::get_theme(game_config_, level["theme"]);

	LOG_NG << "building terrain rules... " << (SDL_GetTicks() - ticks()) << std::endl;
	loadscreen::start_stage("build terrain");
	gui_.reset(new game_display(gamestate().board_, video, whiteboard_manager_, *gamestate().reports_, gamestate().tod_manager_, theme_cfg, level));
	if (!gui_->video().faked()) {
		if (saved_game_.mp_settings().mp_countdown)
			gui_->get_theme().modify_label("time-icon", _ ("time left for current turn"));
		else
			gui_->get_theme().modify_label("time-icon", _ ("current local time"));
	}

	loadscreen::start_stage("init display");
	mouse_handler_.set_gui(gui_.get());
	menu_handler_.set_gui(gui_.get());
	resources::screen = gui_.get();

	LOG_NG << "done initializing display... " << (SDL_GetTicks() - ticks()) << std::endl;

	LOG_NG << "building gamestate to gui and whiteboard... " << (SDL_GetTicks() - ticks()) << std::endl;
	//loadscreen::start_stage("build events manager & lua");
	// This *needs* to be created before the show_intro and show_map_scene
	// as that functions use the manager state_of_game
	// Has to be done before registering any events!
	gamestate().bind(whiteboard_manager_.get(), gui_.get());
	resources::lua_kernel = gamestate().lua_kernel_.get();
	resources::game_events = gamestate().events_manager_.get();

	if(gamestate().first_human_team_ != -1) {
		gui_->set_team(gamestate().first_human_team_);
	}
	else if(is_observer()) {
		// Find first team that is allowed to be observed.
		// If not set here observer would be without fog until
		// the first turn of observable side
		size_t i;
		for (i=0;i < gamestate().board_.teams().size();++i)
		{
			if (!gamestate().board_.teams()[i].get_disallow_observers())
			{
				gui_->set_team(i);
			}
		}
	}

	init_managers();
	loadscreen::global_loadscreen->start_stage("start game");
	loadscreen_manager->reset();

	plugins_context_.reset(new plugins_context("Game"));
	plugins_context_->set_callback("save_game", boost::bind(&play_controller::save_game_auto, this, boost::bind(get_str, _1, "filename" )), true);
	plugins_context_->set_callback("save_replay", boost::bind(&play_controller::save_replay_auto, this, boost::bind(get_str, _1, "filename" )), true);
	plugins_context_->set_callback("quit", throw_end_level(), false);
}

void play_controller::reset_gamestate(const config& level, int replay_pos)
{
	resources::gameboard = NULL;
	resources::gamedata = NULL;
	resources::teams = NULL;
	resources::tod_manager = NULL;
	resources::units = NULL;
	resources::filter_con = NULL;
	resources::lua_kernel = NULL;
	resources::game_events = NULL;
	resources::tunnels = NULL;
	resources::undo_stack = NULL;

	gamestate_.reset(new game_state(level, *this, tdata_));
	resources::gameboard = &gamestate().board_;
	resources::gamedata = &gamestate().gamedata_;
	resources::teams = &gamestate().board_.teams_;
	resources::tod_manager = &gamestate().tod_manager_;
	resources::units = &gamestate().board_.units_;
	resources::filter_con = &gamestate();
	resources::undo_stack = &undo_stack();

	gamestate_->init(level, *this);
	gamestate().bind(whiteboard_manager_.get(), gui_.get());
	resources::lua_kernel = gamestate().lua_kernel_.get();
	resources::game_events = gamestate().events_manager_.get();
	resources::tunnels = gamestate().pathfind_manager_.get();

	gui_->reset_tod_manager(gamestate().tod_manager_);
	gui_->reset_reports(*gamestate().reports_);
	gui_->change_display_context(&gamestate().board_);
	saved_game_.get_replay().set_pos(replay_pos);
}

void play_controller::init_managers()
{
	LOG_NG << "initializing managers... " << (SDL_GetTicks() - ticks()) << std::endl;
	prefs_disp_manager_.reset(new preferences::display_manager(gui_.get()));
	tooltips_manager_.reset(new tooltips::manager(gui_->video()));
	soundsources_manager_.reset(new soundsource::manager(*gui_));

	resources::soundsources = soundsources_manager_.get();
	LOG_NG << "done initializing managers... " << (SDL_GetTicks() - ticks()) << std::endl;
}

void play_controller::fire_preload(const config& level)
{
	// Run initialization scripts, even if loading from a snapshot.
	gamestate().gamedata_.set_phase(game_data::PRELOAD);
	gamestate().lua_kernel_->initialize(level);
	gamestate().gamedata_.get_variable("turn_number") = int(turn());
	pump().fire("preload");
}

void play_controller::fire_prestart()
{
	// pre-start events must be executed before any GUI operation,
	// as those may cause the display to be refreshed.
	update_locker lock_display(gui_->video());
	gamestate().gamedata_.set_phase(game_data::PRESTART);
	pump().fire("prestart");
	// prestart event may modify start turn with WML, reflect any changes.
	gamestate().gamedata_.get_variable("turn_number") = int(turn());
}

void play_controller::fire_start()
{
	gamestate().gamedata_.set_phase(game_data::START);
	pump().fire("start");
	// start event may modify start turn with WML, reflect any changes.
	gamestate().gamedata_.get_variable("turn_number") = int(turn());
	check_objectives();
	// prestart and start events may modify the initial gold amount,
	// reflect any changes.
	BOOST_FOREACH(team& tm, gamestate().board_.teams_)
	{
		tm.set_start_gold(tm.gold());
	}
	gamestate_->init_side_done() = false;
	gamestate().gamedata_.set_phase(game_data::PLAY);
}

void play_controller::init_gui()
{
	gui_->begin_game();
	gui_->update_tod();
}

void play_controller::init_side_begin()
{
	mouse_handler_.set_side(current_side());

	// If we are observers we move to watch next team if it is allowed
	if ((is_observer() && !current_team().get_disallow_observers())
		|| (current_team().is_local_human() && !this->is_replay())) 
	{
		update_gui_to_player(current_side() - 1);
	}

	gui_->set_playing_team(size_t(current_side() - 1));

	gamestate().gamedata_.last_selected = map_location::null_location();
}

void play_controller::maybe_do_init_side()
{
	//
	// We do side init only if not done yet for a local side when we are not replaying.
	// For all other sides it is recorded in replay and replay handler has to handle
	// calling do_init_side() functions.
	//
	if (gamestate_->init_side_done() || !current_team().is_local() || gamestate().gamedata_.phase() != game_data::PLAY || is_replay() || !replay_->at_end()) {
		return;
	}

	resources::recorder->init_side();
	do_init_side();
}

void play_controller::do_init_side()
{
	set_scontext_synced sync;
	log_scope("player turn");
	// In case we might end up calling sync:network during the side turn events,
	// and we don't want do_init_side to be called when a player drops.
	gamestate_->init_side_done() = true;
	init_side_done_now_ = true;

	const std::string turn_num = str_cast(turn());
	const std::string side_num = str_cast(current_side());

	gamestate().gamedata_.get_variable("side_number") = current_side();

	// We might have skipped some sides because they were empty so it is not enough to check for side_num==1
	if(!gamestate().tod_manager_.has_turn_event_fired())
	{
		pump().fire("turn " + turn_num);
		pump().fire("new turn");
		gamestate().tod_manager_.turn_event_fired();
	}

	pump().fire("side turn");
	pump().fire("side " + side_num + " turn");
	pump().fire("side turn " + turn_num);
	pump().fire("side " + side_num + " turn " + turn_num);

	// We want to work out if units for this player should get healed,
	// and the player should get income now.
	// Healing/income happen if it's not the first turn of processing,
	// or if we are loading a game.
	if (turn() > 1) {
		gamestate().board_.new_turn(current_side());
		current_team().new_turn();

		// If the expense is less than the number of villages owned
		// times the village support capacity,
		// then we don't have to pay anything at all
		int expense = gamestate().board_.side_upkeep(current_side()) -
			current_team().support();
		if(expense > 0) {
			current_team().spend_gold(expense);
		}

		calculate_healing(current_side(), !is_skipping_replay());
	}

	// Prepare the undo stack.
	undo_stack().new_side_turn(current_side());

	pump().fire("turn refresh");
	pump().fire("side " + side_num + " turn refresh");
	pump().fire("turn " + turn_num + " refresh");
	pump().fire("side " + side_num + " turn " + turn_num + " refresh");

	// Make sure vision is accurate.
	actions::clear_shroud(current_side(), true);
	init_side_end();
	check_victory();
	sync.do_final_checkup();
}

void play_controller::init_side_end()
{
	const time_of_day& tod = gamestate().tod_manager_.get_time_of_day();

	if (current_side() == 1 || !init_side_done_now_)
		sound::play_sound(tod.sounds, sound::SOUND_SOURCES);

	if (!is_skipping_replay()){
		gui_->invalidate_all();
	}

	if (!is_skipping_replay() && current_team().get_scroll_to_leader()){
		gui_->scroll_to_leader(current_side(), game_display::ONSCREEN,false);
	}
	whiteboard_manager_->on_init_side();
}

config play_controller::to_config() const
{
	config cfg = level_;

	cfg["replay_pos"] = saved_game_.get_replay().get_pos();
	gamestate().write(cfg);

	gui_->write(cfg.add_child("display"));

	//Write the soundsources.
	soundsources_manager_->write_sourcespecs(cfg);
	
	if(gui_.get() != NULL) {
		gui_->labels().write(cfg);
		sound::write_music_play_list(cfg);
	}

	return cfg;
}

void play_controller::finish_side_turn()
{
	whiteboard_manager_->on_finish_side_turn(current_side());

	{ //Block for set_scontext_synced
		set_scontext_synced sync(1);
		// Ending the turn commits all moves.
		undo_stack().clear();
		gamestate().board_.end_turn(current_side());
		const std::string turn_num = str_cast(turn());
		const std::string side_num = str_cast(current_side());

		// Clear shroud, in case units had been slowed for the turn.
		actions::clear_shroud(current_side());

		pump().fire("side turn end");
		pump().fire("side "+ side_num + " turn end");
		pump().fire("side turn " + turn_num + " end");
		pump().fire("side " + side_num + " turn " + turn_num + " end");
		// This is where we refog, after all of a side's events are done.
		actions::recalculate_fog(current_side());
		check_victory();	
		sync.do_final_checkup();
	}

	mouse_handler_.deselect_hex();
	resources::gameboard->unit_id_manager().reset_fake();
	gamestate_->init_side_done() = false;
}

void play_controller::finish_turn()
{
	set_scontext_synced sync(2);
	const std::string turn_num = str_cast(turn());
	pump().fire("turn end");
	pump().fire("turn " + turn_num + " end");
	sync.do_final_checkup();
}

bool play_controller::enemies_visible() const
{
	// If we aren't using fog/shroud, this is easy :)
	if(current_team().uses_fog() == false && current_team().uses_shroud() == false)
		return true;

	// See if any enemies are visible
	BOOST_FOREACH(const unit& u, gamestate().board_.units()) {
		if (current_team().is_enemy(u.side()) && !gui_->fogged(u.get_location())) {
			return true;
		}
	}

	return false;
}

void play_controller::enter_textbox()
{
	if(menu_handler_.get_textbox().active() == false) {
		return;
	}

	const std::string str = menu_handler_.get_textbox().box()->text();
	const unsigned int team_num = current_side();
	events::mouse_handler& mousehandler = mouse_handler_;

	switch(menu_handler_.get_textbox().mode()) {
	case gui::TEXTBOX_SEARCH:
		menu_handler_.do_search(str);
		menu_handler_.get_textbox().close(*gui_);
		break;
	case gui::TEXTBOX_MESSAGE:
		menu_handler_.do_speak();
		menu_handler_.get_textbox().close(*gui_);  //need to close that one after executing do_speak() !
		break;
	case gui::TEXTBOX_COMMAND:
		menu_handler_.get_textbox().close(*gui_);
		menu_handler_.do_command(str);
		break;
	case gui::TEXTBOX_AI:
		menu_handler_.get_textbox().close(*gui_);
		menu_handler_.do_ai_formula(str, team_num, mousehandler);
		break;
	default:
		menu_handler_.get_textbox().close(*gui_);
		ERR_DP << "unknown textbox mode" << std::endl;
	}
}

void play_controller::tab()
{
	gui::TEXTBOX_MODE mode = menu_handler_.get_textbox().mode();

	std::set<std::string> dictionary;
	switch(mode) {
	case gui::TEXTBOX_SEARCH:
	{
		BOOST_FOREACH(const unit& u, gamestate().board_.units()){
			const map_location& loc = u.get_location();
			if(!gui_->fogged(loc) &&
					!(gamestate().board_.teams()[gui_->viewing_team()].is_enemy(u.side()) && u.invisible(loc)))
				dictionary.insert(u.name());
		}
		//TODO List map labels
		break;
	}
	case gui::TEXTBOX_COMMAND:
	{
		std::vector<std::string> commands = menu_handler_.get_commands_list();
		dictionary.insert(commands.begin(), commands.end());
		// no break here, we also want player names from the next case
	}
	case gui::TEXTBOX_MESSAGE:
	{
		BOOST_FOREACH(const team& t, gamestate().board_.teams()) {
			if(!t.is_empty())
				dictionary.insert(t.current_player());
		}

		// Add observers
		BOOST_FOREACH(const std::string& o, gui_->observers()){
			dictionary.insert(o);
		}
		
		// Add nicks who whispered you
		BOOST_FOREACH(const std::string& w, gui_->get_chat_manager().whisperers()){
			dictionary.insert(w);
		}
		
		//Exclude own nick from tab-completion.
		//NOTE why ?
		dictionary.erase(preferences::login());
		break;
	}

	default:
		ERR_DP << "unknown textbox mode" << std::endl;
	} //switch(mode)

	menu_handler_.get_textbox().tab(dictionary);
}

team& play_controller::current_team()
{
	assert(current_side() > 0 && current_side() <= int(gamestate().board_.teams().size()));
	return gamestate().board_.teams_[current_side() - 1];
}

const team& play_controller::current_team() const
{
	assert(current_side() > 0 && current_side() <= int(gamestate().board_.teams().size()));
	return gamestate().board_.teams()[current_side() - 1];
}

/// @returns: the number n in [min, min+mod ) so that (n - num) is a multiple of mod.
static int modulo(int num, int mod, int min)
{
	assert(mod > 0);
	int n = (num - min) % mod;
	if (n < 0)
		n += mod;
	//n is now in [0, mod)
	n = n + min;
	return n;
	// the folowing properties are easy to verify:
	//  1) For all m: modulo(num, mod, min) == modulo(num + mod*m, mod, min)
	//  2) For all 0 <= m < mod: modulo(min + m, mod, min) == min + m
}

bool play_controller::is_team_visible(int team_num, bool observer) const
{
	const team& t = gamestate().board_.teams()[team_num - 1];
	if(observer) {
		return !t.get_disallow_observers() && !t.is_empty();
	}
	else {
		return t.is_local_human() && !t.is_idle();
	}
}

int play_controller::find_last_visible_team() const
{
	assert(current_side() <= int(gamestate().board_.teams().size()));
	const int num_teams = gamestate().board_.teams().size();
	const bool is_observer = this->is_observer();

	for(int i = 0; i < num_teams; i++) {
		const int team_num = modulo(current_side() - i, num_teams, 1);
		if(is_team_visible(team_num, is_observer)) {
			return team_num;
		}
	}
	return 0;
}

events::mouse_handler& play_controller::get_mouse_handler_base()
{
	return mouse_handler_;
}

boost::shared_ptr<wb::manager> play_controller::get_whiteboard()
{
	return whiteboard_manager_;
}

const mp_game_settings& play_controller::get_mp_settings()
{
	return saved_game_.mp_settings();
}

const game_classification& play_controller::get_classification()
{
	return saved_game_.classification();
}

game_display& play_controller::get_display()
{
	return *gui_;
}

bool play_controller::have_keyboard_focus()
{
	return !menu_handler_.get_textbox().active();
}

void play_controller::process_focus_keydown_event(const SDL_Event& event)
{
	if(event.key.keysym.sym == SDLK_ESCAPE) {
		menu_handler_.get_textbox().close(*gui_);
	} else if(event.key.keysym.sym == SDLK_TAB) {
		tab();
	} else if(event.key.keysym.sym == SDLK_RETURN || event.key.keysym.sym == SDLK_KP_ENTER) {
		enter_textbox();
	}
}

void play_controller::process_keydown_event(const SDL_Event& event)
{
	if (event.key.keysym.sym == SDLK_TAB) {
		whiteboard_manager_->set_invert_behavior(true);
	}
}

void play_controller::process_keyup_event(const SDL_Event& event)
{
	// If the user has pressed 1 through 9, we want to show
	// how far the unit can move in that many turns
	if(event.key.keysym.sym >= '1' && event.key.keysym.sym <= '7') {
		const int new_path_turns = (event.type == SDL_KEYDOWN) ?
		                           event.key.keysym.sym - '1' : 0;

		if(new_path_turns != mouse_handler_.get_path_turns()) {
			mouse_handler_.set_path_turns(new_path_turns);

			const unit_map::iterator u = mouse_handler_.selected_unit();

			if(u.valid()) {
				// if it's not the unit's turn, we reset its moves
				unit_movement_resetter move_reset(*u, u->side() != current_side());

				mouse_handler_.set_current_paths(pathfind::paths(*u, false,
				                       true, gamestate().board_.teams_[gui_->viewing_team()],
				                       mouse_handler_.get_path_turns()));

				gui_->highlight_reach(mouse_handler_.current_paths());
			} else {
				mouse_handler_.select_hex(mouse_handler_.get_selected_hex(), false, false, false);
			}

		}
	} else if (event.key.keysym.sym == SDLK_TAB) {
		static CKey keys;
		if (!keys[SDLK_TAB]) {
			whiteboard_manager_->set_invert_behavior(false);
		}
	}
}

void play_controller::save_game()
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		scoped_savegame_snapshot snapshot(*this);
		savegame::ingame_savegame save(saved_game_, *gui_, preferences::save_compression_format());
		save.save_game_interactive(gui_->video(), "", gui::OK_CANCEL);
	} else {
		save_blocker::on_unblock(this,&play_controller::save_game);
	}
}

void play_controller::save_game_auto(const std::string& filename)
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;

		scoped_savegame_snapshot snapshot(*this);
		savegame::ingame_savegame save(saved_game_, *gui_, preferences::save_compression_format());
		save.save_game_automatic(gui_->video(), false, filename);
	}
}

void play_controller::save_replay()
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		savegame::replay_savegame save(saved_game_, preferences::save_compression_format());
		save.save_game_interactive(gui_->video(), "", gui::OK_CANCEL);
	} else {
		save_blocker::on_unblock(this,&play_controller::save_replay);
	}
}

void play_controller::save_replay_auto(const std::string& filename)
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		savegame::replay_savegame save(saved_game_, preferences::save_compression_format());
		save.save_game_automatic(gui_->video(), false, filename);
	}
}

void play_controller::save_map()
{
	if(save_blocker::try_block()) {
		save_blocker::save_unblocker unblocker;
		menu_handler_.save_map();
	} else {
		save_blocker::on_unblock(this,&play_controller::save_map);
	}
}

void play_controller::load_game()
{
	savegame::loadgame load(*gui_, game_config_, saved_game_);
	load.load_game();
}

void play_controller::undo()
{
	mouse_handler_.deselect_hex();
	undo_stack().undo();
}

void play_controller::redo()
{
	mouse_handler_.deselect_hex();
	undo_stack().redo();
}

bool play_controller::can_undo() const
{
	return !linger_ && !is_browsing() && !events::commands_disabled && undo_stack().can_undo();
}

bool play_controller::can_redo() const
{
	return !linger_ && !is_browsing() && !events::commands_disabled && undo_stack().can_redo();
}

namespace {
	static const std::string empty_str = "";
}

const std::string& play_controller::select_victory_music() const
{
	if(victory_music_.empty())
		return empty_str;
	return victory_music_[rand() % victory_music_.size()];
}

const std::string& play_controller::select_defeat_music() const
{
	if(defeat_music_.empty())
		return empty_str;
	return defeat_music_[rand() % defeat_music_.size()];
}

void play_controller::set_victory_music_list(const std::string& list)
{
	victory_music_ = utils::split(list);
	if(victory_music_.empty())
		victory_music_ = utils::split(game_config::default_victory_music);
}

void play_controller::set_defeat_music_list(const std::string& list)
{
	defeat_music_  = utils::split(list);
	if(defeat_music_.empty())
		defeat_music_ = utils::split(game_config::default_defeat_music);
}

void play_controller::check_victory()
{
	if(linger_)
	{
		return;
	}

	if (is_regular_game_end()) {
		return;
	}
	bool continue_level, found_player, found_network_player, invalidate_all;
	std::set<unsigned> not_defeated;

	gamestate().board_.check_victory(continue_level, found_player, found_network_player, invalidate_all, not_defeated, remove_from_carryover_on_defeat_);

	if (invalidate_all) {
		gui_->invalidate_all();
	}

	if (continue_level) {
		return ;
	}

	if (found_player || found_network_player) {
		pump().fire("enemies defeated");
		if (is_regular_game_end()) {
			return;
		}
	}

	DBG_EE << "victory_when_enemies_defeated: " << victory_when_enemies_defeated_ << std::endl;
	DBG_EE << "found_player: " << found_player << std::endl;
	DBG_EE << "found_network_player: " << found_network_player << std::endl;

	if (!victory_when_enemies_defeated_ && (found_player || found_network_player)) {
		// This level has asked not to be ended by this condition.
		return;
	}

	if (non_interactive()) {
		LOG_AIT << "winner: ";
		BOOST_FOREACH(unsigned l, not_defeated) {
			std::string ai = ai::manager::get_active_ai_identifier_for_side(l);
			if (ai.empty()) ai = "default ai";
			LOG_AIT << l << " (using " << ai << ") ";
		}
		LOG_AIT << std::endl;
		ai_testing::log_victory(not_defeated);
	}

	DBG_EE << "throwing end level exception..." << std::endl;
	//Also proceed to the next scenario when another player survived.
	end_level_data el_data;
	el_data.proceed_to_next_level = found_player || found_network_player;
	el_data.is_victory = found_player;
	set_end_level_data(el_data);
}

void play_controller::process_oos(const std::string& msg) const
{
	if (non_interactive()) {
		throw game::game_error(msg);
	}
	if (game_config::ignore_replay_errors) return;

	std::stringstream message;
	message << _("The game is out of sync. It might not make much sense to continue. Do you want to save your game?");
	message << "\n\n" << _("Error details:") << "\n\n" << msg;

	scoped_savegame_snapshot snapshot(*this);
	savegame::oos_savegame save(saved_game_, *gui_);
	save.save_game_interactive(gui_->video(), message.str(), gui::YES_NO); // can throw quit_game_exception
}

void play_controller::update_gui_to_player(const int team_index, const bool observe)
{
	gui_->set_team(team_index, observe);
	gui_->recalculate_minimap();
	gui_->invalidate_all();
	gui_->draw(true,true);
}

void play_controller::do_autosave()
{
	scoped_savegame_snapshot snapshot(*this);
	savegame::autosave_savegame save(saved_game_, *gui_, preferences::save_compression_format());
	save.autosave(false, preferences::autosavemax(), preferences::INFINITE_AUTO_SAVES);
}

void play_controller::do_consolesave(const std::string& filename)
{
	scoped_savegame_snapshot snapshot(*this);
	savegame::ingame_savegame save(saved_game_, *gui_, preferences::save_compression_format());
	save.save_game_automatic(gui_->video(), true, filename);
}

void play_controller::update_savegame_snapshot() const
{
	//note: this writes to level_ if this is not a replay.
	this->saved_game_.set_snapshot(to_config());
}

game_events::t_pump& play_controller::pump()
{
	return gamestate().events_manager_->pump();
}

int play_controller::get_ticks()
{
	return ticks_;
}

soundsource::manager* play_controller::get_soundsource_man()
{
	return soundsources_manager_.get();
}

plugins_context* play_controller::get_plugins_context()
{
	return plugins_context_.get();
}

hotkey::command_executor* play_controller::get_hotkey_command_executor()
{
	return hotkey_handler_.get();
}

bool play_controller::is_browsing() const
{
	if(linger_ || !gamestate_->init_side_done() || gamestate().gamedata_.phase() != game_data::PLAY) {
		return true;
	}
	const team& t = current_team();
	return !t.is_local_human() || !t.is_proxy_human();
}

void play_controller::play_slice_catch()
{
	if(should_return_to_play_side()) {
		return;
	}
	try
	{
		play_slice();
	}
	catch(const return_to_play_side_exception&)
	{
		assert(should_return_to_play_side());
	}
}

void play_controller::start_game(const config& level)
{
	fire_preload(level);

	if(!gamestate().start_event_fired_)
	{
		gamestate().start_event_fired_ = true;
		resources::recorder->add_start_if_not_there_yet();
		resources::recorder->get_next_action();
		
		set_scontext_synced sync;

		fire_prestart();
		if (is_regular_game_end()) {
			return;
		}

		for ( int side = gamestate().board_.teams().size(); side != 0; --side )
			actions::clear_shroud(side, false, false);
		
		init_gui();
		LOG_NG << "first_time..." << (is_skipping_replay() ? "skipping" : "no skip") << "\n";

		events::raise_draw_event();
		fire_start();
		if (is_regular_game_end()) {
			return;
		}
		sync.do_final_checkup();
		gui_->recalculate_minimap();
		// Initialize countdown clock.
		BOOST_FOREACH(const team& t, gamestate().board_.teams())
		{
			if (saved_game_.mp_settings().mp_countdown) {
				t.set_countdown_time(1000 * saved_game_.mp_settings().mp_countdown_init_time);
			}
		}
	}
	else
	{
		init_gui();
		events::raise_draw_event();
		gamestate().gamedata_.set_phase(game_data::PLAY);
		gui_->recalculate_minimap();
	}
}

bool play_controller::can_use_synced_wml_menu() const
{
	const team& viewing_team = get_teams_const()[gui_->viewing_team()];
	return gui_->viewing_team() == gui_->playing_team() && !events::commands_disabled && viewing_team.is_local_human() && !is_lingering() && !is_browsing();
}

std::set<std::string> play_controller::all_players() const
{
	std::set<std::string> res = gui_->observers();
	BOOST_FOREACH(const team& t, get_teams_const())
	{
		if (t.is_human()) {
			res.insert(t.current_player());
		}
	}
	return res;
}

void play_controller::play_side()
{
	//check for team-specific items in the scenario
	gui_->parse_team_overlays();
	do {
		update_viewing_player();
		{ 
			save_blocker blocker;
			maybe_do_init_side();
			if(is_regular_game_end()) {
				return;
			}
		}
		// This flag can be set by derived classes (in overridden functions).
		player_type_changed_ = false;

		statistics::reset_turn_stats(gamestate().board_.teams()[current_side() - 1].save_id());

		play_side_impl();

		if(is_regular_game_end()) {
			return;
		}

	} while (player_type_changed_);
	// Keep looping if the type of a team (human/ai/networked)
	// has changed mid-turn
	sync_end_turn();
}

void play_controller::play_turn()
{
	whiteboard_manager_->on_gamestate_change();
	gui_->new_turn();
	gui_->invalidate_game_status();
	events::raise_draw_event();

	LOG_NG << "turn: " << turn() << "\n";

	if(non_interactive()) {
		LOG_AIT << "Turn " << turn() << ":" << std::endl;
	}

	for (; gamestate_->player_number_ <= int(gamestate().board_.teams().size()); ++gamestate_->player_number_)
	{
		// If a side is empty skip over it.
		if (current_team().is_empty()) {
			continue;
		}
		init_side_begin();
		if(gamestate_->init_side_done()) {
			// This is the case in a reloaded game where the side was initialized before saving the game.
			init_side_end();
		}

		ai_testing::log_turn_start(current_side());
		play_side();
		if(is_regular_game_end()) {
			return;
		}
		finish_side_turn();
		if(is_regular_game_end()) {
			return;
		}
		if(non_interactive()) {
			LOG_AIT << " Player " << current_side() << ": " <<
				current_team().villages().size() << " Villages" <<
				std::endl;
			ai_testing::log_turn_end(current_side());
		}
	}
	// If the loop exits due to the last team having been processed.
	gamestate_->player_number_ = gamestate().board_.teams().size();

	finish_turn();

	// Time has run out
	check_time_over();
}

void play_controller::check_time_over()
{
	const bool time_left = gamestate().tod_manager_.next_turn(gamestate().gamedata_);

	if(!time_left) {
		LOG_NG << "firing time over event...\n";
		set_scontext_synced_base sync;
		pump().fire("time over");
		LOG_NG << "done firing time over event...\n";
		// If turns are added while handling 'time over' event.
		if (gamestate().tod_manager_.is_time_left()) {
			return;
		}

		if(non_interactive()) {
			LOG_AIT << "time over (draw)\n";
			ai_testing::log_draw();
		}

		check_victory();
		if (is_regular_game_end()) {
			return;
		}
		end_level_data e;
		e.proceed_to_next_level = false;
		e.is_victory = false;
		set_end_level_data(e);
	}
}

play_controller::scoped_savegame_snapshot::scoped_savegame_snapshot(const play_controller& controller)
	: controller_(controller)
{
	controller_.update_savegame_snapshot();
}

play_controller::scoped_savegame_snapshot::~scoped_savegame_snapshot()
{
	controller_.saved_game_.remove_snapshot();
}
