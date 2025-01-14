/*
   Copyright (C) 2003 - 2018 by David White <dave@whitevine.net>
   Copyright (C) 2015 - 2020 by Iris Morelle <shadowm2006@gmail.com>
   Part of the Battle for Wesnoth Project https://www.wesnoth.org/

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY.

   See the COPYING file for more details.
   */

/**
 * @file
 * Wesnoth addon server.
 * Expects a "server.cfg" config file in the current directory
 * and saves addons under data/.
 */

#include "server/campaignd/server.hpp"

#include "filesystem.hpp"
#include "lexical_cast.hpp"
#include "log.hpp"
#include "serialization/base64.hpp"
#include "serialization/binary_or_text.hpp"
#include "serialization/parser.hpp"
#include "serialization/string_utils.hpp"
#include "serialization/unicode.hpp"
#include "game_config.hpp"
#include "addon/validation.hpp"
#include "server/campaignd/addon_utils.hpp"
#include "server/campaignd/auth.hpp"
#include "server/campaignd/blacklist.hpp"
#include "server/campaignd/control.hpp"
#include "server/campaignd/fs_commit.hpp"
#include "server/campaignd/options.hpp"
#include "game_version.hpp"
#include "hash.hpp"
#include "utils/optimer.hpp"

#include <csignal>
#include <ctime>
#include <iomanip>

// the fork execute is unix specific only tested on Linux quite sure it won't
// work on Windows not sure which other platforms have a problem with it.
#if !(defined(_WIN32))
#include <errno.h>
#endif

static lg::log_domain log_campaignd("campaignd");
#define DBG_CS LOG_STREAM(debug, log_campaignd)
#define LOG_CS LOG_STREAM(info,  log_campaignd)
#define WRN_CS LOG_STREAM(warn,  log_campaignd)
#define ERR_CS LOG_STREAM(err,   log_campaignd)

static lg::log_domain log_config("config");
#define ERR_CONFIG LOG_STREAM(err, log_config)
#define WRN_CONFIG LOG_STREAM(warn, log_config)

static lg::log_domain log_server("server");
#define ERR_SERVER LOG_STREAM(err, log_server)

#include "server/common/send_receive_wml_helpers.ipp"

namespace campaignd {

namespace {

bool timing_reports_enabled = false;

void timing_report_function(const util::ms_optimer& tim, const campaignd::server::request& req, const std::string& label = {})
{
	if(timing_reports_enabled) {
		if(label.empty()) {
			LOG_CS << req << "Time elapsed: " << tim << " ms\n";
		} else {
			LOG_CS << req << "Time elapsed [" << label << "]: " << tim << " ms\n";
		}
	}
}

inline util::ms_optimer service_timer(const campaignd::server::request& req, const std::string& label = {})
{
	return util::ms_optimer{std::bind(timing_report_function, std::placeholders::_1, req, label)};
}

//
// Auxiliary shortcut functions
//

/**
 * WML version of campaignd::auth::verify_passphrase().
 *
 * The salt and hash are retrieved from the @a passsalt and @a passhash
 * attributes, respectively.
 */
inline bool authenticate(config& addon, const config::attribute_value& passphrase)
{
	return auth::verify_passphrase(passphrase, addon["passsalt"], addon["passhash"]);
}

/**
 * WML version of campaignd::auth::generate_hash().
 *
 * The salt and hash are written into the @a passsalt and @a passhash
 * attributes, respectively.
 */
inline void set_passphrase(config& addon, const std::string& passphrase)
{
	std::tie(addon["passsalt"], addon["passhash"]) = auth::generate_hash(passphrase);
}

/**
 * Returns the update pack filename for the specified old/new version pair.
 *
 * The filename is in the form @p "update_pack_<VERSION_MD5>.gz".
 */
inline std::string make_update_pack_filename(const std::string& old_version, const std::string& new_version)
{
	return "update_pack_" + utils::md5(old_version + new_version).hex_digest() + ".gz";
}

/**
 * Returns the full pack filename for the specified version.
 *
 * The filename is in the form @p "full_pack_<VERSION_MD5>.gz".
 */
inline std::string make_full_pack_filename(const std::string& version)
{
	return "full_pack_" + utils::md5(version).hex_digest() + ".gz";
}

/**
 * Returns the index filename for the specified version.
 *
 * The filename is in the form @p "full_pack_<VERSION_MD5>.hash.gz".
 */
inline std::string make_index_filename(const std::string& version)
{
	return "full_pack_" + utils::md5(version).hex_digest() + ".hash.gz";
}

/**
 * Returns the index counterpart for the specified full pack file.
 *
 * The result is in the same form as make_index_filename().
 */
inline std::string index_from_full_pack_filename(std::string pack_fn)
{
	auto dot_pos = pack_fn.find_last_of('.');
	if(dot_pos != std::string::npos) {
		pack_fn.replace(dot_pos, std::string::npos, ".hash.gz");
	}
	return pack_fn;
}

/**
 * Returns a pointer to a WML child if it exists or nullptr otherwise.
 */
const config* optional_wml_child(const config& cfg, const std::string& child_name)
{
	return cfg.has_child(child_name) ? &cfg.child(child_name) : nullptr;
}

/**
 * Returns @a false if @a cfg is null or empty.
 */
bool have_wml(const config* cfg)
{
	return cfg && !cfg->empty();
}

/**
 * Scans multiple WML pack-like trees for illegal names.
 *
 * Null WML objects are skipped.
 */
bool multi_find_illegal_names(std::vector<std::string>& names, const std::vector<const config*>& indices)
{
	names.clear();

	for(auto* index : indices) {
		if(index) {
			check_names_legal(*index, &names);
		}
	}

	return !names.empty();
}

/**
 * Scans multiple WML pack-like trees for case conflicts.
 *
 * Null WML objects are skipped.
 */
bool multi_find_case_conflicts(std::vector<std::string>& names, const std::vector<const config*>& indices)
{
	names.clear();

	for(auto* index : indices) {
		if(index) {
			check_case_insensitive_duplicates(*index, &names);
		}
	}

	return !names.empty();
}

/**
 * Escapes double quotes intended to be passed into simple_wml.
 *
 * Just why does simple_wml have to be so broken to force us to use this, though?
 */
std::string simple_wml_escape(const std::string& text)
{
	std::string res;
	auto it = text.begin();

	while(it != text.end()) {
		res.append(*it == '"' ? 2 : 1, *it);
		++it;
	}

	return res;
}

} // end anonymous namespace

server::server(const std::string& cfg_file, unsigned short port)
	: server_base(default_campaignd_port, true)
	, addons_()
	, dirty_addons_()
	, cfg_()
	, cfg_file_(cfg_file)
	, read_only_(false)
	, compress_level_(0)
	, update_pack_lifespan_(0)
	, hooks_()
	, handlers_()
	, feedback_url_format_()
	, blacklist_()
	, blacklist_file_()
	, stats_exempt_ips_()
	, flush_timer_(io_service_)
{

#ifndef _WIN32
	struct sigaction sa;
	std::memset( &sa, 0, sizeof(sa) );
	#pragma GCC diagnostic ignored "-Wold-style-cast"
	sa.sa_handler = SIG_IGN;
	int res = sigaction( SIGPIPE, &sa, nullptr);
	assert( res == 0 );
#endif
	load_config();

	// Command line config override. This won't get saved back to disk since we
	// leave the WML intentionally untouched.
	if(port != 0) {
		port_ = port;
	}

	LOG_CS << "Port: " << port_ << '\n';
	LOG_CS << "Server directory: " << game_config::path << " (" << addons_.size() << " add-ons)\n";

	if(!read_only_) {
		// Migrate old add-ons to use hashed passphrases (1.12+)
		for(auto& entry : addons_) {
			auto& id = entry.first;
			auto& addon = entry.second;

			// Add-on already has a hashed password
			if(addon["passphrase"].empty()) {
				continue;
			}

			LOG_CS << "Addon '" << addon["title"] << "' uses unhashed passphrase. Fixing.\n";
			set_passphrase(addon, addon["passphrase"]);
			addon["passphrase"] = "";
			mark_dirty(id);
		}
		write_config();
	}

	register_handlers();

	start_server();
	flush_cfg();
}

server::~server()
{
	write_config();
}

void server::load_config()
{
	LOG_CS << "Reading configuration from " << cfg_file_ << "...\n";

	filesystem::scoped_istream in = filesystem::istream_file(cfg_file_);
	read(cfg_, *in);

	read_only_ = cfg_["read_only"].to_bool(false);

	if(read_only_) {
		LOG_CS << "READ-ONLY MODE ACTIVE\n";
	}

	// Seems like compression level above 6 is a waste of CPU cycles.
	compress_level_ = cfg_["compress_level"].to_int(6);
	// One month probably will be fine (#TODO: testing needed)
	update_pack_lifespan_ = cfg_["update_pack_lifespan"].to_time_t(30 * 24 * 60 * 60);

	const config& svinfo_cfg = server_info();
	if(svinfo_cfg) {
		feedback_url_format_ = svinfo_cfg["feedback_url_format"].str();
	}

	blacklist_file_ = cfg_["blacklist_file"].str();
	load_blacklist();

	stats_exempt_ips_ = utils::split(cfg_["stats_exempt_ips"].str());

	// Load any configured hooks.
	hooks_.emplace(std::string("hook_post_upload"), cfg_["hook_post_upload"]);
	hooks_.emplace(std::string("hook_post_erase"), cfg_["hook_post_erase"]);

#ifndef _WIN32
	// Open the control socket if enabled.
	if(!cfg_["control_socket"].empty()) {
		const std::string& path = cfg_["control_socket"].str();

		if(path != fifo_path_) {
			const int res = mkfifo(path.c_str(),0660);
			if(res != 0 && errno != EEXIST) {
				ERR_CS << "could not make fifo at '" << path << "' (" << strerror(errno) << ")\n";
			} else {
				input_.close();
				int fifo = open(path.c_str(), O_RDWR|O_NONBLOCK);
				input_.assign(fifo);
				LOG_CS << "opened fifo at '" << path << "'. Server commands may be written to this file.\n";
				read_from_fifo();
				fifo_path_ = path;
			}
		}
	}
#endif

	// Certain config values are saved to WML again so that a given server
	// instance's parameters remain constant even if the code defaults change
	// at some later point.
	cfg_["compress_level"] = compress_level_;

	// But not the listening port number.
	port_ = cfg_["port"].to_int(default_campaignd_port);

	// Limit the max size of WML documents received from the net to prevent the
	// possible excessive use of resources due to malformed packets received.
	// Since an addon is sent in a single WML document this essentially limits
	// the maximum size of an addon that can be uploaded.
	simple_wml::document::document_size_limit = cfg_["document_size_limit"].to_int(default_document_size_limit);

	//Loading addons
	addons_.clear();
	std::vector<std::string> legacy_addons, dirs;
	filesystem::get_files_in_dir("data", &legacy_addons, &dirs);
	config meta;
	for(const std::string& addon_dir : dirs) {
		in = filesystem::istream_file(filesystem::normalize_path("data/" + addon_dir + "/addon.cfg"));
		read(meta, *in);
		if(!meta.empty()) {
			addons_.emplace(meta["name"].str(), meta);
		} else {
			throw filesystem::io_exception("Failed to load addon from dir '" + addon_dir + "'\n");
		}
	}

	// Convert all legacy addons to the new format on load
	if(cfg_.has_child("campaigns")) {
		config& campaigns = cfg_.child("campaigns");
		WRN_CS << "Old format addons have been detected in the config! They will be converted to the new file format! "
		       << campaigns.child_count("campaign") << " entries to be processed.\n";
		for(config& campaign : campaigns.child_range("campaign")) {
			const std::string& addon_id = campaign["name"].str();
			const std::string& addon_file = campaign["filename"].str();
			if(get_addon(addon_id)) {
				throw filesystem::io_exception("The addon '" + addon_id
					   + "' already exists in the new form! Possible code or filesystem interference!\n");
			}
			if(std::find(legacy_addons.begin(), legacy_addons.end(), addon_id) == legacy_addons.end()) {
				throw filesystem::io_exception("No file has been found for the legacy addon '" + addon_id
					   + "'. Check the file structure!\n");
			}

			config data;
			in = filesystem::istream_file(filesystem::normalize_path(addon_file));
			read_gz(data, *in);
			if(!data) {
				throw filesystem::io_exception("Couldn't read the content file for the legacy addon '" + addon_id + "'!\n");
			}

			config version_cfg = config("version", campaign["version"].str());
			version_cfg["filename"] = make_full_pack_filename(campaign["version"]);
			campaign.add_child("version", version_cfg);

			data.remove_attributes("title", "campaign_name", "author", "description", "version", "timestamp", "original_timestamp", "icon", "type", "tags");
			filesystem::delete_file(filesystem::normalize_path(addon_file));
			{
				filesystem::atomic_commit campaign_file(addon_file + "/" + version_cfg["filename"].str());
				config_writer writer(*campaign_file.ostream(), true, compress_level_);
				writer.write(data);
				campaign_file.commit();
			}
			{
				filesystem::atomic_commit campaign_hash_file(addon_file + "/" + make_index_filename(campaign["version"]));
				config_writer writer(*campaign_hash_file.ostream(), true, compress_level_);
				config data_hash = config("name", "");
				write_hashlist(data_hash, data);
				writer.write(data_hash);
				campaign_hash_file.commit();
			}

			addons_.emplace(addon_id, campaign);
			mark_dirty(addon_id);
		}
		cfg_.clear_children("campaigns");
		LOG_CS << "Legacy addons processing finished.\n";
		write_config();
	}

	LOG_CS << "Loaded addons metadata. " << addons_.size() << " addons found.\n";
}

std::ostream& operator<<(std::ostream& o, const server::request& r)
{
	o << '[' << r.addr << ' ' << r.cmd << "] ";
	return o;
}

void server::handle_new_client(socket_ptr socket)
{
	async_receive_doc(socket,
					  std::bind(&server::handle_request, this, _1, _2)
					  );
}

void server::handle_request(socket_ptr socket, std::shared_ptr<simple_wml::document> doc)
{
	config data;
	read(data, doc->output());

	config::all_children_iterator i = data.ordered_begin();

	if(i != data.ordered_end()) {
		// We only handle the first child.
		const config::any_child& c = *i;

		request_handlers_table::const_iterator j
				= handlers_.find(c.key);

		if(j != handlers_.end()) {
			// Call the handler.
			request req{c.key, c.cfg, socket};
			auto st = service_timer(req);
			j->second(this, req);
		} else {
			send_error("Unrecognized [" + c.key + "] request.",socket);
		}
	}
}

#ifndef _WIN32

void server::handle_read_from_fifo(const boost::system::error_code& error, std::size_t)
{
	if(error) {
		if(error == boost::asio::error::operation_aborted)
			// This means fifo was closed by load_config() to open another fifo
			return;
		ERR_CS << "Error reading from fifo: " << error.message() << '\n';
		return;
	}

	std::istream is(&admin_cmd_);
	std::string cmd;
	std::getline(is, cmd);

	const control_line ctl = cmd;

	if(ctl == "shut_down") {
		LOG_CS << "Shut down requested by admin, shutting down...\n";
		throw server_shutdown("Shut down via fifo command");
	} else if(ctl == "readonly") {
		if(ctl.args_count()) {
			cfg_["read_only"] = read_only_ = utils::string_bool(ctl[1], true);
		}

		LOG_CS << "Read only mode: " << (read_only_ ? "enabled" : "disabled") << '\n';
	} else if(ctl == "flush") {
		LOG_CS << "Flushing config to disk...\n";
		write_config();
	} else if(ctl == "reload") {
		if(ctl.args_count()) {
			if(ctl[1] == "blacklist") {
				LOG_CS << "Reloading blacklist...\n";
				load_blacklist();
			} else {
				ERR_CS << "Unrecognized admin reload argument: " << ctl[1] << '\n';
			}
		} else {
			LOG_CS << "Reloading all configuration...\n";
			load_config();
			LOG_CS << "Reloaded configuration\n";
		}
	} else if(ctl == "delete") {
		if(ctl.args_count() != 1) {
			ERR_CS << "Incorrect number of arguments for 'delete'\n";
		} else {
			const std::string& addon_id = ctl[1];

			LOG_CS << "deleting add-on '" << addon_id << "' requested from control FIFO\n";
			delete_addon(addon_id);
		}
	} else if(ctl == "hide" || ctl == "unhide") {
		if(ctl.args_count() != 1) {
			ERR_CS << "Incorrect number of arguments for '" << ctl.cmd() << "'\n";
		} else {
			const std::string& addon_id = ctl[1];
			config& addon = get_addon(addon_id);

			if(!addon) {
				ERR_CS << "Add-on '" << addon_id << "' not found, cannot " << ctl.cmd() << "\n";
			} else {
				addon["hidden"] = ctl.cmd() == "hide";
				mark_dirty(addon_id);
				write_config();
				LOG_CS << "Add-on '" << addon_id << "' is now " << (ctl.cmd() == "hide" ? "hidden" : "unhidden") << '\n';
			}
		}
	} else if(ctl == "setpass") {
		if(ctl.args_count() != 2) {
			ERR_CS << "Incorrect number of arguments for 'setpass'\n";
		} else {
			const std::string& addon_id = ctl[1];
			const std::string& newpass = ctl[2];
			config& addon = get_addon(addon_id);

			if(!addon) {
				ERR_CS << "Add-on '" << addon_id << "' not found, cannot set passphrase\n";
			} else if(newpass.empty()) {
				// Shouldn't happen!
				ERR_CS << "Add-on passphrases may not be empty!\n";
			} else {
				set_passphrase(addon, newpass);
				mark_dirty(addon_id);
				write_config();
				LOG_CS << "New passphrase set for '" << addon_id << "'\n";
			}
		}
	} else if(ctl == "setattr") {
		if(ctl.args_count() != 3) {
			ERR_CS << "Incorrect number of arguments for 'setattr'\n";
		} else {
			const std::string& addon_id = ctl[1];
			const std::string& key = ctl[2];
			const std::string& value = ctl[3];

			config& addon = get_addon(addon_id);

			if(!addon) {
				ERR_CS << "Add-on '" << addon_id << "' not found, cannot set attribute\n";
			} else if(key == "name" || key == "version") {
				ERR_CS << "setattr cannot be used to rename add-ons or change their version\n";
			} else if(key == "passphrase" || key == "passhash"|| key == "passsalt") {
				ERR_CS << "setattr cannot be used to set auth data -- use setpass instead\n";
			} else if(!addon.has_attribute(key)) {
				// NOTE: This is a very naive approach for validating setattr's
				//       input, but it should generally work since add-on
				//       uploads explicitly set all recognized attributes to
				//       the values provided by the .pbl data or the empty
				//       string if absent, and this is normally preserved by
				//       the config serialization.
				ERR_CS << "Attribute '" << value << "' is not a recognized add-on attribute\n";
			} else {
				addon[key] = value;
				mark_dirty(addon_id);
				write_config();
				LOG_CS << "Set attribute on add-on '" << addon_id << "':\n"
				       << key << "=\"" << value << "\"\n";
			}
		}
	} else {
		ERR_CS << "Unrecognized admin command: " << ctl.full() << '\n';
	}

	read_from_fifo();
}

void server::handle_sighup(const boost::system::error_code&, int)
{
	LOG_CS << "SIGHUP caught, reloading config.\n";

	load_config(); // TODO: handle port number config changes

	LOG_CS << "Reloaded configuration\n";

	sighup_.async_wait(std::bind(&server::handle_sighup, this, _1, _2));
}

#endif

void server::flush_cfg()
{
	flush_timer_.expires_from_now(std::chrono::minutes(10));
	flush_timer_.async_wait(std::bind(&server::handle_flush, this, _1));
}

void server::handle_flush(const boost::system::error_code& error)
{
	if(error) {
		ERR_CS << "Error from reload timer: " << error.message() << "\n";
		throw boost::system::system_error(error);
	}
	write_config();
	flush_cfg();
}

void server::load_blacklist()
{
	// We *always* want to clear the blacklist first, especially if we are
	// reloading the configuration and the blacklist is no longer enabled.
	blacklist_.clear();

	if(blacklist_file_.empty()) {
		return;
	}

	try {
		filesystem::scoped_istream in = filesystem::istream_file(blacklist_file_);
		config blcfg;

		read(blcfg, *in);

		blacklist_.read(blcfg);
		LOG_CS << "using blacklist from " << blacklist_file_ << '\n';
	} catch(const config::error&) {
		ERR_CS << "failed to read blacklist from " << blacklist_file_ << ", blacklist disabled\n";
	}
}

void server::write_config()
{
	DBG_CS << "writing configuration and add-ons list to disk...\n";
	filesystem::atomic_commit out(cfg_file_);
	write(*out.ostream(), cfg_);
	out.commit();

	for(const std::string& name : dirty_addons_) {
		const config& addon = get_addon(name);
		if(addon && !addon["filename"].empty()) {
			filesystem::atomic_commit addon_out(filesystem::normalize_path(addon["filename"].str() + "/addon.cfg"));
			write(*addon_out.ostream(), addon);
			addon_out.commit();
		}
	}

	dirty_addons_.clear();
	DBG_CS << "... done\n";
}

void server::fire(const std::string& hook, const std::string& addon)
{
	const std::map<std::string, std::string>::const_iterator itor = hooks_.find(hook);
	if(itor == hooks_.end()) {
		return;
	}

	const std::string& script = itor->second;
	if(script.empty()) {
		return;
	}

#if defined(_WIN32)
	UNUSED(addon);
	ERR_CS << "Tried to execute a script on an unsupported platform\n";
	return;
#else
	pid_t childpid;

	if((childpid = fork()) == -1) {
		ERR_CS << "fork failed while updating add-on " << addon << '\n';
		return;
	}

	if(childpid == 0) {
		// We are the child process. Execute the script. We run as a
		// separate thread sharing stdout/stderr, which will make the
		// log look ugly.
		execlp(script.c_str(), script.c_str(), addon.c_str(), static_cast<char *>(nullptr));

		// exec() and family never return; if they do, we have a problem
		std::cerr << "ERROR: exec failed with errno " << errno << " for addon " << addon
				  << '\n';
		exit(errno);

	} else {
		return;
	}
#endif
}

bool server::ignore_address_stats(const std::string& addr) const
{
	for(const auto& mask : stats_exempt_ips_) {
		// TODO: we want CIDR subnet mask matching here, not glob matching!
		if(utils::wildcard_string_match(addr, mask)) {
			return true;
		}
	}

	return false;
}

void server::send_message(const std::string& msg, socket_ptr sock)
{
	const auto& escaped_msg = simple_wml_escape(msg);
	simple_wml::document doc;
	doc.root().add_child("message").set_attr_dup("message", escaped_msg.c_str());
	async_send_doc(sock, doc, std::bind(&server::handle_new_client, this, _1), null_handler);
}

void server::send_error(const std::string& msg, socket_ptr sock)
{
	ERR_CS << "[" << client_address(sock) << "] " << msg << '\n';
	const auto& escaped_msg = simple_wml_escape(msg);
	simple_wml::document doc;
	doc.root().add_child("error").set_attr_dup("message", escaped_msg.c_str());
	async_send_doc(sock, doc, std::bind(&server::handle_new_client, this, _1), null_handler);
}

void server::send_error(const std::string& msg, const std::string& extra_data, unsigned int status_code, socket_ptr sock)
{
	const std::string& status_hex = formatter()
		<< "0x" << std::setfill('0') << std::setw(2*sizeof(unsigned int)) << std::hex
		<< std::uppercase << status_code;
	ERR_CS << "[" << client_address(sock) << "]: (" << status_hex << ") " << msg << '\n';

	const auto& escaped_status_str = simple_wml_escape(std::to_string(status_code));
	const auto& escaped_msg = simple_wml_escape(msg);
	const auto& escaped_extra_data = simple_wml_escape(extra_data);

	simple_wml::document doc;
	simple_wml::node& err_cfg = doc.root().add_child("error");

	err_cfg.set_attr_dup("message", escaped_msg.c_str());
	err_cfg.set_attr_dup("extra_data", escaped_extra_data.c_str());
	err_cfg.set_attr_dup("status_code", escaped_status_str.c_str());

	async_send_doc(sock, doc, std::bind(&server::handle_new_client, this, _1), null_handler);
}

config& server::get_addon(const std::string& id)
{
	auto addon = addons_.find(id);
	if(addon != addons_.end()) {
		return addon->second;
	} else {
		return config::get_invalid();
	}
}

void server::delete_addon(const std::string& id)
{
	config& cfg = get_addon(id);

	if(!cfg) {
		ERR_CS << "Cannot delete unrecognized add-on '" << id << "'\n";
		return;
	}

	std::string fn = cfg["filename"].str();

	if(fn.empty()) {
		ERR_CS << "Add-on '" << id << "' does not have an associated filename, cannot delete\n";
	}

	if(!filesystem::delete_directory(fn)) {
		ERR_CS << "Could not delete the directory for addon '" << id
		       << "' (" << fn << "): " << strerror(errno) << '\n';
	}

	addons_.erase(id);
	write_config();

	fire("hook_post_erase", id);

	LOG_CS << "Deleted add-on '" << id << "'\n";
}

#define REGISTER_CAMPAIGND_HANDLER(req_id) \
	handlers_[#req_id] = std::bind(&server::handle_##req_id, \
		std::placeholders::_1, std::placeholders::_2)

void server::register_handlers()
{
	REGISTER_CAMPAIGND_HANDLER(request_campaign_list);
	REGISTER_CAMPAIGND_HANDLER(request_campaign);
	REGISTER_CAMPAIGND_HANDLER(request_campaign_hash);
	REGISTER_CAMPAIGND_HANDLER(request_terms);
	REGISTER_CAMPAIGND_HANDLER(upload);
	REGISTER_CAMPAIGND_HANDLER(delete);
	REGISTER_CAMPAIGND_HANDLER(change_passphrase);
}

void server::handle_request_campaign_list(const server::request& req)
{
	LOG_CS << req << "Sending add-ons list\n";

	std::time_t epoch = std::time(nullptr);
	config addons_list;

	addons_list["timestamp"] = epoch;
	if(req.cfg["times_relative_to"] != "now") {
		epoch = 0;
	}

	bool before_flag = false;
	std::time_t before = epoch;
	if(!req.cfg["before"].empty()) {
		before += req.cfg["before"].to_time_t();
		before_flag = true;
	}

	bool after_flag = false;
	std::time_t after = epoch;
	if(!req.cfg["after"].empty()) {
		after += req.cfg["after"].to_time_t();
		after_flag = true;
	}

	const std::string& name = req.cfg["name"];
	const std::string& lang = req.cfg["language"];

	for(const auto& addon : addons_)
	{
		if(!name.empty() && name != addon.first) {
			continue;
		}

		config i = addon.second;

		if(i["hidden"].to_bool()) {
			continue;
		}

		const auto& tm = i["timestamp"];

		if(before_flag && (tm.empty() || tm.to_time_t(0) >= before)) {
			continue;
		}
		if(after_flag && (tm.empty() || tm.to_time_t(0) <= after)) {
			continue;
		}

		if(!lang.empty()) {
			bool found = false;

			for(const config& j : i.child_range("translation"))
			{
				if(j["language"] == lang && j["supported"].to_bool(true)) {//for old addons
					found = true;
					break;
				}
			}

			if(!found) {
				continue;
			}
		}

		addons_list.add_child("campaign", i);
	}

	for(config& j : addons_list.child_range("campaign"))
	{
		// Remove attributes containing information that's considered sensitive
		// or irrelevant to clients
		j.remove_attributes("passphrase", "passhash", "passsalt", "upload_ip", "email");

		// Build a feedback_url string attribute from the internal [feedback]
		// data or deliver an empty value, in case clients decide to assume its
		// presence.
		const config& url_params = j.child_or_empty("feedback");
		j["feedback_url"] = !url_params.empty() && !feedback_url_format_.empty()
							? format_addon_feedback_url(feedback_url_format_, url_params) : "";

		// Clients don't need to see the original data, so discard it.
		j.clear_children("feedback");

		// Update packs info is internal stuff
		j.clear_children("update_pack");
	}

	config response;
	response.add_child("campaigns", std::move(addons_list));

	std::ostringstream ostr;
	write(ostr, response);
	std::string wml = ostr.str();
	simple_wml::document doc(wml.c_str(), simple_wml::INIT_STATIC);
	doc.compress();

	async_send_doc(req.sock, doc, std::bind(&server::handle_new_client, this, _1));
}

void server::handle_request_campaign(const server::request& req)
{
	config& addon = get_addon(req.cfg["name"]);

	if(!addon || addon["hidden"].to_bool()) {
		send_error("Add-on '" + req.cfg["name"].str() + "' not found.", req.sock);
		return;
	}

	const auto& name = req.cfg["name"].str();
	auto version_map = get_version_map(addon);

	if(version_map.empty()) {
		send_error("No versions of the add-on '" + name + "' are available on the server.", req.sock);
		return;
	}

	// Base the payload against the latest version if no particular version is being requested
	const auto& from = req.cfg["from_version"].str();
	const auto& to = req.cfg["version"].str(version_map.rbegin()->first);

	auto to_version_iter = version_map.find(version_info{to});
	if(to_version_iter == version_map.end()) {
		send_error("Could not find requested version " + to + " of the addon '" + name +
					"'.", req.sock);
		return;
	}

	auto full_pack_path = addon["filename"].str() + '/' + to_version_iter->second["filename"].str();
	const int full_pack_size = filesystem::file_size(full_pack_path);

	if(!from.empty() && version_map.count(version_info{from}) != 0) {
		// Build a sequence of updates beginning from the client's old version to the
		// requested version. Every pair of incrementing versions on the server should
		// have an update pack written to disk during the original upload(s).
		//
		// TODO: consider merging update packs instead of building a linear
		// and possibly redundant sequence out of them.

		config delta;
		int delivery_size = 0;
		bool force_use_full = false;

		auto start_point = version_map.find(version_info{from}); // Already known to exist
		auto end_point = std::next(to_version_iter, 1); // May be end()

		if(std::distance(start_point, end_point) <= 1) {
			// This should not happen, skip the sequence build entirely
			ERR_CS << "Bad update sequence bounds in version " << from << " -> " << to << " update sequence for the add-on '" << name << "', sending a full pack instead\n";
			force_use_full = true;
		}

		for(auto iter = start_point; !force_use_full && std::distance(iter, end_point) > 1;) {
			const auto& prev_version_cfg = iter->second;
			const auto& next_version_cfg = (++iter)->second;

			for(const config& pack : addon.child_range("update_pack")) {
				if(pack["from"].str() != prev_version_cfg["version"].str() ||
				   pack["to"].str() != next_version_cfg["version"].str()) {
					continue;
				}

				config step_delta;
				const auto& update_pack_path = addon["filename"].str() + '/' + pack["filename"].str();
				auto in = filesystem::istream_file(update_pack_path);

				read_gz(step_delta, *in);

				if(!step_delta.empty()) {
					// Don't copy arbitrarily large data around
					delta.append(std::move(step_delta));
					delivery_size += filesystem::file_size(update_pack_path);
				} else {
					ERR_CS << "Broken update sequence from version " << from << " to "
							<< to << " for the add-on '" << name << "', sending a full pack instead\n";
					force_use_full = true;
					break;
				}

				// No point in sending an overlarge delta update.
				// FIXME: This doesn't take into account over-the-wire compression
				// from async_send_doc() though, maybe some heuristics based on
				// individual update pack size would be useful?
				if(delivery_size > full_pack_size && full_pack_size > 0) {
					force_use_full = true;
					break;
				}
			}
		}

		if(!force_use_full && !delta.empty()) {
			std::ostringstream ostr;
			write(ostr, delta);
			const auto& wml_text = ostr.str();

			simple_wml::document doc(wml_text.c_str(), simple_wml::INIT_STATIC);
			doc.compress();

			LOG_CS << req << "Sending add-on '" << name << "' version: " << from << " -> " << to << " (delta))\n";

			async_send_doc(req.sock, doc, std::bind(&server::handle_new_client, this, _1), null_handler);

			full_pack_path.clear();
		}
	}

	// Send a full pack if the client's previous version was not specified, is
	// not known by the server, or if any other condition above caused us to
	// give up on the update pack option.
	if(!full_pack_path.empty()) {
		if(full_pack_size < 0) {
			send_error("Add-on '" + name + "' could not be read by the server.", req.sock);
			return;
		}

		LOG_CS << req << "Sending add-on '" << name << "' version: " << to << " size: " << full_pack_size / 1024 << " KiB\n";
		async_send_file(req.sock, full_pack_path, std::bind(&server::handle_new_client, this, _1), null_handler);
	}

	// Clients doing upgrades or some other specific thing shouldn't bump
	// the downloads count. Default to true for compatibility with old
	// clients that won't tell us what they are trying to do.
	if(from.empty() && req.cfg["increase_downloads"].to_bool(true) && !ignore_address_stats(req.addr)) {
		addon["downloads"] = 1 + addon["downloads"].to_int();
		mark_dirty(name);
	}
}

void server::handle_request_campaign_hash(const server::request& req)
{
	config& addon = get_addon(req.cfg["name"]);

	if(!addon || addon["hidden"].to_bool()) {
		send_error("Add-on '" + req.cfg["name"].str() + "' not found.", req.sock);
		return;
	}

	std::string path = addon["filename"].str() + '/';

	auto version_map = get_version_map(addon);

	if(version_map.empty()) {
		send_error("No versions of the add-on '" + req.cfg["name"].str() + "' are available on the server.", req.sock);
		return;
	} else {
		const auto& version_str = addon["version"].str();
		version_info version_parsed{version_str};
		auto version = version_map.find(version_parsed);
		if(version != version_map.end()) {
			path += version->second["filename"].str();
		} else {
			// Selecting the latest version before the selected version or the overall latest version if unspecified
			if(version_str.empty()) {
				path += version_map.rbegin()->second["filename"].str();
			} else {
				path += (--version_map.upper_bound(version_parsed))->second["filename"].str();
			}
		}

		path = index_from_full_pack_filename(path);
		const int file_size = filesystem::file_size(path);

		if(file_size < 0) {
			send_error("Missing index file for the add-on '" + req.cfg["name"].str() + "'.", req.sock);
			return;
		}

		LOG_CS << req << "Sending add-on hash index for '" << req.cfg["name"] << "' size: " << file_size / 1024 << " KiB\n";
		async_send_file(req.sock, path, std::bind(&server::handle_new_client, this, _1), null_handler);
	}
}

void server::handle_request_terms(const server::request& req)
{
	// This usually means the client wants to upload content, so tell it
	// to give up when we're in read-only mode.
	if(read_only_) {
		LOG_CS << "in read-only mode, request for upload terms denied\n";
		send_error("The server is currently in read-only mode, add-on uploads are disabled.", req.sock);
		return;
	}

	// TODO: possibly move to server.cfg
	static const std::string terms = R"""(All content within add-ons uploaded to this server must be licensed under the terms of the GNU General Public License (GPL), with the sole exception of graphics and audio explicitly denoted as released under a Creative Commons license either in:

    a) a combined toplevel file, e.g. “My_Addon/ART_LICENSE”; <b>or</b>
    b) a file with the same path as the asset with “.license” appended, e.g. “My_Addon/images/units/axeman.png.license”.

<b>By uploading content to this server, you certify that you have the right to:</b>

    a) release all included art and audio explicitly denoted with a Creative Commons license in the proscribed manner under that license; <b>and</b>
    b) release all other included content under the terms of the GPL; and that you choose to do so.)""";

	LOG_CS << req << "Sending license terms\n";
	send_message(terms, req.sock);
}

ADDON_CHECK_STATUS server::validate_addon(const server::request& req, config*& existing_addon, std::string& error_data)
{
	if(read_only_) {
		LOG_CS << "Validation error: uploads not permitted in read-only mode.\n";
		return ADDON_CHECK_STATUS::SERVER_READ_ONLY;
	}

	const config& upload = req.cfg;

	const config* data =  optional_wml_child(upload, "data");
	const config* removelist = optional_wml_child(upload, "removelist");
	const config* addlist = optional_wml_child(upload, "addlist");

	const bool is_upload_pack = have_wml(removelist) || have_wml(addlist);

	const std::string& name = upload["name"].str();

	existing_addon = nullptr;
	error_data.clear();

	bool passed_name_utf8_check = false;

	try {
		const std::string& lc_name = utf8::lowercase(name);
		passed_name_utf8_check = true;

		for(auto& c : addons_) {
			if(utf8::lowercase(c.first) == lc_name) {
				existing_addon = &c.second;
				break;
			}
		}
	} catch(const utf8::invalid_utf8_exception&) {
		if(!passed_name_utf8_check) {
			LOG_CS << "Validation error: bad UTF-8 in add-on name\n";
			return ADDON_CHECK_STATUS::INVALID_UTF8_NAME;
		} else {
			ERR_CS << "Validation error: add-ons list has bad UTF-8 somehow, this is a server side issue, it's bad, and you should probably fix it ASAP\n";
			return ADDON_CHECK_STATUS::SERVER_ADDONS_LIST;
		}
	}

	// Auth and block-list based checks go first

	if(upload["passphrase"].empty()) {
		LOG_CS << "Validation error: no passphrase specified\n";
		return ADDON_CHECK_STATUS::NO_PASSPHRASE;
	}

	if(existing_addon && !authenticate(*existing_addon, upload["passphrase"])) {
		LOG_CS << "Validation error: passphrase does not match\n";
		return ADDON_CHECK_STATUS::UNAUTHORIZED;
	}

	if(existing_addon && (*existing_addon)["hidden"].to_bool()) {
		LOG_CS << "Validation error: add-on is hidden\n";
		return ADDON_CHECK_STATUS::DENIED;
	}

	try {
		if(blacklist_.is_blacklisted(name,
									 upload["title"].str(),
									 upload["description"].str(),
									 upload["author"].str(),
									 req.addr,
									 upload["email"].str()))
		{
			LOG_CS << "Validation error: blacklisted uploader or publish information\n";
			return ADDON_CHECK_STATUS::DENIED;
		}
	} catch(const utf8::invalid_utf8_exception&) {
		LOG_CS << "Validation error: invalid UTF-8 sequence in publish information while checking against the blacklist\n";
		return ADDON_CHECK_STATUS::INVALID_UTF8_ATTRIBUTE;
	}

	// Structure and syntax checks follow

	if(!is_upload_pack && !have_wml(data)) {
		LOG_CS << "Validation error: no add-on data.\n";
		return ADDON_CHECK_STATUS::EMPTY_PACK;
	}

	if(is_upload_pack && !have_wml(removelist) && !have_wml(addlist)) {
		LOG_CS << "Validation error: no add-on data.\n";
		return ADDON_CHECK_STATUS::EMPTY_PACK;
	}

	if(!addon_name_legal(name)) {
		LOG_CS << "Validation error: invalid add-on name.\n";
		return ADDON_CHECK_STATUS::BAD_NAME;
	}

	if(is_text_markup_char(name[0])) {
		LOG_CS << "Validation error: add-on name starts with an illegal formatting character.\n";
		return ADDON_CHECK_STATUS::NAME_HAS_MARKUP;
	}

	if(upload["title"].empty()) {
		LOG_CS << "Validation error: no add-on title specified\n";
		return ADDON_CHECK_STATUS::NO_TITLE;
	}

	if(is_text_markup_char(upload["title"].str()[0])) {
		LOG_CS << "Validation error: add-on title starts with an illegal formatting character.\n";
		return ADDON_CHECK_STATUS::TITLE_HAS_MARKUP;
	}

	if(get_addon_type(upload["type"]) == ADDON_UNKNOWN) {
		LOG_CS << "Validation error: unknown add-on type specified\n";
		return ADDON_CHECK_STATUS::BAD_TYPE;
	}

	if(upload["author"].empty()) {
		LOG_CS << "Validation error: no add-on author specified\n";
		return ADDON_CHECK_STATUS::NO_AUTHOR;
	}

	if(upload["version"].empty()) {
		LOG_CS << "Validation error: no add-on version specified\n";
		return ADDON_CHECK_STATUS::NO_VERSION;
	}

	if(upload["description"].empty()) {
		LOG_CS << "Validation error: no add-on description specified\n";
		return ADDON_CHECK_STATUS::NO_DESCRIPTION;
	}

	if(upload["email"].empty()) {
		LOG_CS << "Validation error: no add-on email specified\n";
		return ADDON_CHECK_STATUS::NO_EMAIL;
	}

	std::vector<std::string> badnames;

	if(multi_find_illegal_names(badnames, {data, addlist, removelist})) {
		error_data = utils::join(badnames, "\n");
		LOG_CS << "Validation error: invalid filenames in add-on pack (" << badnames.size() << " entries)\n";
		return ADDON_CHECK_STATUS::ILLEGAL_FILENAME;
	}

	if(multi_find_case_conflicts(badnames, {data, addlist, removelist})) {
		error_data = utils::join(badnames, "\n");
		LOG_CS << "Validation error: case conflicts in add-on pack (" << badnames.size() << " entries)\n";
		return ADDON_CHECK_STATUS::FILENAME_CASE_CONFLICT;
	}

	if(is_upload_pack && !existing_addon) {
		LOG_CS << "Validation error: attempted to send an update pack for a non-existent add-on\n";
		return ADDON_CHECK_STATUS::UNEXPECTED_DELTA;
	}

	return ADDON_CHECK_STATUS::SUCCESS;
}

void server::handle_upload(const server::request& req)
{
	const std::time_t upload_ts = std::time(nullptr);
	const config& upload = req.cfg;
	const auto& name = upload["name"].str();

	LOG_CS << req << "Validating add-on '" << name << "'...\n";

	config* addon_ptr = nullptr;
	std::string val_error_data;
	const auto val_status = validate_addon(req, addon_ptr, val_error_data);

	if(val_status != ADDON_CHECK_STATUS::SUCCESS) {
		LOG_CS << "Upload of '" << name << "' aborted due to a failed validation check\n";
		const auto msg = std::string("Add-on rejected: ") + addon_check_status_desc(val_status);
		send_error(msg, val_error_data, static_cast<unsigned int>(val_status), req.sock);
		return;
	}

	LOG_CS << req << "Processing add-on '" << name << "'...\n";

	const config* const full_pack    = optional_wml_child(upload, "data");
	const config* const delta_remove = optional_wml_child(upload, "removelist");
	const config* const delta_add    = optional_wml_child(upload, "addlist");

	const bool is_delta_upload = have_wml(delta_remove) || have_wml(delta_add);
	const bool is_existing_upload = addon_ptr != nullptr;

	if(!is_existing_upload) {
		// Create a new add-ons list entry and work with that from now on
		auto entry = addons_.emplace(name, config("original_timestamp", upload_ts));
		addon_ptr = &(*entry.first).second;
	}

	config& addon = *addon_ptr;

	LOG_CS << req << "Upload type: "
		   << (is_delta_upload ? "delta" : "full") << ", "
		   << (is_existing_upload ? "update" : "new") << '\n';

	// Write general metadata attributes

	addon.copy_attributes(upload,
		"title", "name", "author", "description", "version", "icon",
		"translate", "dependencies", "type", "tags", "email");

	const std::string& pathstem = "data/" + name;
	addon["filename"] = pathstem;
	addon["upload_ip"] = req.addr;

	if(!is_existing_upload) {
		set_passphrase(addon, upload["passphrase"]);
	}

	if(addon["downloads"].empty()) {
		addon["downloads"] = 0;
	}

	addon["timestamp"] = upload_ts;
	addon["uploads"] = 1 + addon["uploads"].to_int();

	addon.clear_children("feedback");
	if(const config& url_params = upload.child("feedback")) {
		addon.add_child("feedback", url_params);
	}

	// Copy in any metadata translations provided directly in the .pbl.
	// Catalogue detection is done later -- in the meantime we just mark
	// translations with valid metadata as not supported until we find out
	// whether the add-on ships translation catalogues for them or not.

	addon.clear_children("translation");

	for(const config& locale_params : upload.child_range("translation")) {
		if(!locale_params["language"].empty()) {
			config& locale = addon.add_child("translation");
			locale["language"] = locale_params["language"].str();
			locale["supported"] = false;

			if(!locale_params["title"].empty()) {
				locale["title"] = locale_params["title"].str();
			}
			if(!locale_params["description"].empty()) {
				locale["description"] = locale_params["description"].str();
			}
		}
	}

	// We need to alter the WML pack slightly, but we don't want to do a deep
	// copy of data that's larger than 5 MB in the average case (and as large
	// as 100 MB in the worst case). On the other hand, if the upload is a
	// delta then need to leave this empty and fill it in later instead.

	config rw_full_pack;
	if(have_wml(full_pack)) {
		// Void the warranty
		rw_full_pack = std::move(const_cast<config&>(*full_pack));
	}

	// Versioning support

	const auto& new_version = addon["version"].str();
	auto version_map = get_version_map(addon);

	if(is_delta_upload) {
		// Create the full pack by grabbing the one for the requested 'from'
		// version (or latest available) and applying the delta on it. We
		// proceed from there by fill in rw_full_pack with the result.

		if(version_map.empty()) {
			// This should NEVER happen
			ERR_CS << "Add-on '" << name << "' has an empty version table, this should not happen\n";
			send_error("Server error: Cannot process update pack with an empty version table.", "", static_cast<unsigned int>(ADDON_CHECK_STATUS::SERVER_DELTA_NO_VERSIONS), req.sock);
			return;
		}

		auto prev_version = upload["from"].str();

		if(prev_version.empty()) {
			prev_version = version_map.rbegin()->first;
		} else {
			// If the requested 'from' version doesn't exist, select the newest
			// older version available.
			version_info prev_version_parsed{prev_version};
			auto vm_entry = version_map.find(prev_version_parsed);
			if(vm_entry == version_map.end()) {
				prev_version = (--version_map.upper_bound(prev_version_parsed))->first;
			}
		}

		// Remove any existing update packs targeting the new version. This is
		// really only needed if the server allows multiple uploads of an
		// add-on with the same version number.

		std::set<std::string> delete_packs;
		for(const auto& pack : addon.child_range("update_pack")) {
			if(pack["to"].str() == new_version) {
				const auto& pack_filename = pack["filename"].str();
				filesystem::delete_file(pathstem + '/' + pack_filename);
				delete_packs.insert(pack_filename);
			}
		}

		if(!delete_packs.empty()) {
			addon.remove_children("update_pack", [&delete_packs](const config& p) {
				return delete_packs.find(p["filename"].str()) != delete_packs.end();
			});
		}

		const auto& update_pack_fn = make_update_pack_filename(prev_version, new_version);

		config& pack_info = addon.add_child("update_pack");

		pack_info["from"] = prev_version;
		pack_info["to"] = new_version;
		pack_info["expire"] = upload_ts + update_pack_lifespan_;
		pack_info["filename"] = update_pack_fn;

		// Write the update pack to disk

		{
			LOG_CS << "Saving provided update pack for " << prev_version << " -> " << new_version << "...\n";

			filesystem::atomic_commit pack_file{pathstem + '/' + update_pack_fn};
			config_writer writer{*pack_file.ostream(), true, compress_level_};
			static const config empty_config;

			writer.open_child("removelist");
			writer.write(have_wml(delta_remove) ? *delta_remove : empty_config);
			writer.close_child("removelist");

			writer.open_child("addlist");
			writer.write(have_wml(delta_add) ? *delta_add : empty_config);
			writer.close_child("addlist");

			pack_file.commit();
		}

		// Apply it to the addon data from the previous version to generate a
		// new full pack, which will be written later near the end of this
		// request servicing routine.

		version_info prev_version_parsed{prev_version};
		auto it = version_map.find(prev_version_parsed);
		if(it == version_map.end()) {
			// This REALLY should never happen
			ERR_CS << "Previous version dropped off the version map?\n";
			send_error("Server error: Previous version disappeared.", "", static_cast<unsigned int>(ADDON_CHECK_STATUS::SERVER_UNSPECIFIED), req.sock);
			return;
		}

		auto in = filesystem::istream_file(pathstem + '/' + it->second["filename"].str());
		rw_full_pack.clear();
		read_gz(rw_full_pack, *in);

		if(have_wml(delta_remove)) {
			data_apply_removelist(rw_full_pack, *delta_remove);
		}

		if(have_wml(delta_add)) {
			data_apply_addlist(rw_full_pack, *delta_add);
		}
	}

	// Detect translation catalogues and toggle their supported status accordingly

	find_translations(rw_full_pack, addon);

	// Add default license information if needed

	add_license(rw_full_pack);

	// Update version map, first removing any identical existing versions

	version_info new_version_parsed{new_version};
	config version_cfg{"version", new_version};
	version_cfg["filename"] = make_full_pack_filename(new_version);

	version_map.erase(new_version_parsed);
	addon.remove_children("version", [&new_version](const config& old_cfg)
		{
			return old_cfg["version"].str() == new_version;
		}
	);

	version_map.emplace(new_version_parsed, version_cfg);
	addon.add_child("version", version_cfg);

	// Clean-up

	rw_full_pack["name"] = ""; // [dir] syntax expects this to be present and empty

	// Write the full pack and its index file

	const auto& full_pack_path = pathstem + '/' + version_cfg["filename"].str();
	const auto& index_path = pathstem + '/' + make_index_filename(new_version);

	{
		config pack_index{"name", ""}; // [dir] syntax expects this to be present and empty
		write_hashlist(pack_index, rw_full_pack);

		filesystem::atomic_commit addon_pack_file{full_pack_path};
		config_writer{*addon_pack_file.ostream(), true, compress_level_}.write(rw_full_pack);
		addon_pack_file.commit();

		filesystem::atomic_commit addon_index_file{index_path};
		config_writer{*addon_index_file.ostream(), true, compress_level_}.write(pack_index);
		addon_index_file.commit();
	}

	addon["size"] = filesystem::file_size(full_pack_path);

	// Expire old update packs and delete them

	std::set<std::string> expire_packs;

	for(const config& pack : addon.child_range("update_pack")) {
		if(upload_ts > pack["expire"].to_time_t() || pack["from"].str() == new_version || (!is_delta_upload && pack["to"].str() == new_version)) {
			LOG_CS << "Expiring upate pack for " << pack["from"].str() << " -> " << pack["to"].str() << "\n";
			const auto& pack_filename = pack["filename"].str();
			filesystem::delete_file(pathstem + '/' + pack_filename);
			expire_packs.insert(pack_filename);
		}
	}

	if(!expire_packs.empty()) {
		addon.remove_children("update_pack", [&expire_packs](const config& p) {
			return expire_packs.find(p["filename"].str()) != expire_packs.end();
		});
	}

	// Create any missing update packs between consecutive versions. This covers
	// cases where clients were not able to upload those update packs themselves.

	for(auto iter = version_map.begin(); std::distance(iter, version_map.end()) > 1;) {
		const config& prev_version = iter->second;
		const config& next_version = (++iter)->second;

		const auto& prev_version_name = prev_version["version"].str();
		const auto& next_version_name = next_version["version"].str();

		bool found = false;

		for(const auto& pack : addon.child_range("update_pack")) {
			if(pack["from"].str() == prev_version_name && pack["to"].str() == next_version_name) {
				found = true;
				break;
			}
		}

		if(found) {
			// Nothing to do
			continue;
		}

		LOG_CS << "Automatically generating update pack for " << prev_version_name << " -> " << next_version_name << "...\n";

		const auto& prev_path = pathstem + '/' + prev_version["filename"].str();
		const auto& next_path = pathstem + '/' + next_version["filename"].str();

		if(filesystem::file_size(prev_path) <= 0 || filesystem::file_size(next_path) <= 0) {
			ERR_CS << "Unable to automatically generate an update pack for '" << name
					<< "' for version " << prev_version_name << " to " << next_version_name
					<< "!\n";
			continue;
		}

		const auto& update_pack_fn = make_update_pack_filename(prev_version_name, next_version_name);

		config& pack_info = addon.add_child("update_pack");
		pack_info["from"] = prev_version_name;
		pack_info["to"] = next_version_name;
		pack_info["expire"] = upload_ts + update_pack_lifespan_;
		pack_info["filename"] = update_pack_fn;

		// Generate the update pack from both full packs

		config pack, from, to;

		filesystem::scoped_istream in = filesystem::istream_file(prev_path);
		read_gz(from, *in);
		in = filesystem::istream_file(next_path);
		read_gz(to, *in);

		make_updatepack(pack, from, to);

		{
			filesystem::atomic_commit pack_file{pathstem + '/' + update_pack_fn};
			config_writer{*pack_file.ostream(), true, compress_level_}.write(pack);
			pack_file.commit();
		}
	}

	mark_dirty(name);
	write_config();

	LOG_CS << req << "Finished uploading add-on '" << upload["name"] << "'\n";

	send_message("Add-on accepted.", req.sock);

	fire("hook_post_upload", name);
}

void server::handle_delete(const server::request& req)
{
	const config& erase = req.cfg;
	const std::string& id = erase["name"].str();

	if(read_only_) {
		LOG_CS << req << "in read-only mode, request to delete '" << id << "' denied\n";
		send_error("Cannot delete add-on: The server is currently in read-only mode.", req.sock);
		return;
	}

	LOG_CS << req << "Deleting add-on '" << id << "'\n";

	config& addon = get_addon(id);

	if(!addon) {
		send_error("The add-on does not exist.", req.sock);
		return;
	}

	const config::attribute_value& pass = erase["passphrase"];

	if(pass.empty()) {
		send_error("No passphrase was specified.", req.sock);
		return;
	}

	if(!authenticate(addon, pass)) {
		send_error("The passphrase is incorrect.", req.sock);
		return;
	}

	if(addon["hidden"].to_bool()) {
		LOG_CS << "Add-on removal denied - hidden add-on.\n";
		send_error("Add-on deletion denied. Please contact the server administration for assistance.", req.sock);
		return;
	}

	delete_addon(id);

	send_message("Add-on deleted.", req.sock);
}

void server::handle_change_passphrase(const server::request& req)
{
	const config& cpass = req.cfg;

	if(read_only_) {
		LOG_CS << "in read-only mode, request to change passphrase denied\n";
		send_error("Cannot change passphrase: The server is currently in read-only mode.", req.sock);
		return;
	}

	config& addon = get_addon(cpass["name"]);

	if(!addon) {
		send_error("No add-on with that name exists.", req.sock);
	} else if(!authenticate(addon, cpass["passphrase"])) {
		send_error("Your old passphrase was incorrect.", req.sock);
	} else if(addon["hidden"].to_bool()) {
		LOG_CS << "Passphrase change denied - hidden add-on.\n";
		send_error("Add-on passphrase change denied. Please contact the server administration for assistance.", req.sock);
	} else if(cpass["new_passphrase"].empty()) {
		send_error("No new passphrase was supplied.", req.sock);
	} else {
		set_passphrase(addon, cpass["new_passphrase"]);
		dirty_addons_.emplace(addon["name"]);
		write_config();
		send_message("Passphrase changed.", req.sock);
	}
}

} // end namespace campaignd

int run_campaignd(int argc, char** argv)
{
	campaignd::command_line cmdline{argc, argv};
	std::string server_path = filesystem::get_cwd();
	std::string config_file = "server.cfg";
	unsigned short port = 0;

	//
	// Log defaults
	//

	for(auto domain : { "campaignd", "campaignd/blacklist", "server" }) {
		lg::set_log_domain_severity(domain, lg::info());
	}

	lg::timestamps(true);

	//
	// Process command line
	//

	if(cmdline.help) {
		std::cout << cmdline.help_text();
		return 0;
	}

	if(cmdline.version) {
		std::cout << "Wesnoth campaignd v" << game_config::revision << '\n';
		return 0;
	}

	if(cmdline.config_file) {
		// Don't fully resolve the path, so that filesystem::ostream_file() can
		// create path components as needed (dumb legacy behavior).
		config_file = filesystem::normalize_path(*cmdline.config_file, true, false);
	}

	if(cmdline.server_dir) {
		server_path = filesystem::normalize_path(*cmdline.server_dir, true, true);
	}

	if(cmdline.port) {
		port = *cmdline.port;
		// We use 0 as a placeholder for the default port for this version
		// otherwise, hence this check must only exists in this code path. It's
		// only meant to protect against user mistakes.
		if(!port) {
			std::cerr << "Invalid network port: " << port << '\n';
			return 2;
		}
	}

	if(cmdline.show_log_domains) {
		std::cout << lg::list_logdomains("");
		return 0;
	}

	for(const auto& ldl : cmdline.log_domain_levels) {
		if(!lg::set_log_domain_severity(ldl.first, ldl.second)) {
			std::cerr << "Unknown log domain: " << ldl.first << '\n';
			return 2;
		}
	}

	if(cmdline.log_precise_timestamps) {
		lg::precise_timestamps(true);
	}

	if(cmdline.report_timings) {
		campaignd::timing_reports_enabled = true;
	}

	std::cerr << "Wesnoth campaignd v" << game_config::revision << " starting...\n";

	if(server_path.empty() || !filesystem::is_directory(server_path)) {
		std::cerr << "Server directory '" << *cmdline.server_dir << "' does not exist or is not a directory.\n";
		return 1;
	}

	if(filesystem::is_directory(config_file)) {
		std::cerr << "Server configuration file '" << config_file << "' is not a file.\n";
		return 1;
	}

	// Everything does file I/O with pwd as the implicit starting point, so we
	// need to change it accordingly. We don't do this before because paths in
	// the command line need to remain relative to the original pwd.
	if(cmdline.server_dir && !filesystem::set_cwd(server_path)) {
		std::cerr << "Bad server directory '" << server_path << "'.\n";
		return 1;
	}

	game_config::path = server_path;

	//
	// Run the server
	//
	campaignd::server(config_file, port).run();

	return 0;
}

int main(int argc, char** argv)
{
	try {
		run_campaignd(argc, argv);
	} catch(const boost::program_options::error& e) {
		std::cerr << "Error in command line: " << e.what() << '\n';
		return 10;
	} catch(const config::error& /*e*/) {
		std::cerr << "Could not parse config file\n";
		return 1;
	} catch(const filesystem::io_exception& e) {
		std::cerr << "File I/O error: " << e.what() << "\n";
		return 2;
	} catch(const std::bad_function_call& /*e*/) {
		std::cerr << "Bad request handler function call\n";
		return 4;
	}

	return 0;
}
