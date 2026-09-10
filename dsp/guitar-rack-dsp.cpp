// guitar-rack-dsp: monolithic DSP child process built from Guitarix engine
// sources (no GTK UI, no dynamic plugin loading). It is spawned by the
// guitar-rack supervisor, connects to JACK/PipeWire as a single-client with
// mono input + stereo output ports, loads a guitarix bank/preset, and runs
// the exact same GxEngine processing chains as the guitarix binary.
//
// Ports (single client mode, see GxJack::gx_jack_init):
//   <instance>:in_0      audio input  (mono)
//   <instance>:out_0     audio output left
//   <instance>:out_1     audio output right
//   <instance>:midi_in_1 midi input
//
// SPDX-License-Identifier: GPL-3.0-or-later

#include <glibmm/init.h>
#include <giomm/init.h>
#include <glibmm/dispatcher.h>
#include <glibmm/main.h>

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "engine.h"
#include "gx_engine.h"
#include "gx_jack.h"
#include "gx_preset.h"

namespace {

void write_json_string(std::ostream& out, const std::string& value) {
    out << '"';
    for (unsigned char c : value) {
        switch (c) {
        case '"': out << "\\\""; break;
        case '\\': out << "\\\\"; break;
        case '\n': out << "\\n"; break;
        case '\r': out << "\\r"; break;
        case '\t': out << "\\t"; break;
        default:
            if (c < 0x20) {
                const char hex[] = "0123456789abcdef";
                out << "\\u00" << hex[c >> 4] << hex[c & 0xf];
            } else {
                out << c;
            }
        }
    }
    out << '"';
}

bool ends_with(const std::string& value, const char* suffix) {
    const std::size_t size = std::strlen(suffix);
    return value.size() >= size && value.compare(value.size() - size, size, suffix) == 0;
}

bool unit_matches(const std::string& parameter_id, const std::string& prefix,
                  const char** groups) {
    if (parameter_id.compare(0, prefix.size(), prefix) == 0) {
        return true;
    }
    if (!groups) {
        return false;
    }
    for (const char** group = groups; *group; group += 2) {
        const char* id = *group;
        if (id[0] != '.') {
            continue;
        }
        ++id;
        const std::size_t size = std::strlen(id);
        if (parameter_id.size() > size && parameter_id.compare(0, size, id) == 0
            && parameter_id[size] == '.') {
            return true;
        }
    }
    return false;
}

bool editable_parameter(gx_engine::Parameter* parameter) {
    const std::string& id = parameter->id();
    return !parameter->isOutput()
        && parameter->isInPreset()
        && parameter->getControlType() != gx_engine::Parameter::None
        && (parameter->isFloat() || parameter->isInt() || parameter->isBool())
        && id != "amp.out_master"
        && !ends_with(id, ".on_off")
        && !ends_with(id, ".position")
        && !ends_with(id, ".pp")
        && !ends_with(id, ".s_h");
}

std::vector<gx_engine::Parameter*> module_parameters(
    gx_engine::ParamMap& parameters, gx_engine::Plugin* plugin) {
    std::vector<gx_engine::Parameter*> result;
    PluginDef* definition = plugin->get_pdef();
    const std::string prefix = std::string(definition->id) + ".";
    for (auto it = parameters.begin(); it != parameters.end(); ++it) {
        gx_engine::Parameter* parameter = it->second;
        if (editable_parameter(parameter)
            && unit_matches(parameter->id(), prefix, definition->groups)) {
            result.push_back(parameter);
        }
    }
    return result;
}

double parameter_value(gx_engine::Parameter* parameter) {
    if (parameter->isFloat()) {
        return parameter->getFloat().get_value();
    }
    if (parameter->isInt()) {
        return parameter->getInt().get_value();
    }
    return parameter->getBool().get_value() ? 1.0 : 0.0;
}

void write_parameter(std::ostream& out, gx_engine::Parameter* parameter,
                     std::size_t index) {
    const bool boolean = parameter->isBool();
    const double value = parameter_value(parameter);
    const double minimum = boolean ? 0.0
        : parameter->hasRange() ? parameter->getLowerAsFloat() : 0.0;
    const double maximum = boolean ? 1.0
        : parameter->hasRange() ? parameter->getUpperAsFloat() : 1.0;
    const double step = boolean ? 1.0 : parameter->getStepAsFloat();
    std::string name = parameter->l_name();
    if (name.empty()) {
        name = parameter->id().substr(parameter->id().find_last_of('.') + 1);
    }
    out << "{\"index\":" << index << ",\"name\":";
    write_json_string(out, name);
    out << ",\"symbol\":";
    write_json_string(out, parameter->id());
    out << ",\"value\":" << value
        << ",\"default\":" << value
        << ",\"minimum\":" << minimum
        << ",\"maximum\":" << maximum
        << ",\"step\":" << step
        << ",\"hints\":" << (boolean ? 1 : 0) << '}';
}

void write_module(std::ostream& out, gx_engine::ParamMap& parameters,
                  gx_engine::Plugin* plugin) {
    PluginDef* definition = plugin->get_pdef();
    const auto controls = module_parameters(parameters, plugin);
    out << "{\"id\":";
    write_json_string(out, definition->id);
    out << ",\"uri\":";
    write_json_string(out, std::string("guitarix-internal:") + definition->id);
    out << ",\"name\":";
    write_json_string(out, definition->name ? definition->name : definition->id);
    out << ",\"bypassed\":" << (plugin->get_on_off() ? "false" : "true")
        << ",\"fixed\":false,\"parameters\":[";
    for (std::size_t index = 0; index < controls.size(); ++index) {
        if (index) out << ',';
        write_parameter(out, controls[index], index);
    }
    out << "],\"values\":{}}";
}

void send_snapshot(gx_engine::GxEngine& engine, gx_preset::GxSettings& settings,
                   const std::string& bank, const std::string& preset) {
    gx_engine::ParamMap& parameters = engine.get_param();
    double master = 0.0;
    if (parameters.hasId("amp.out_master")) {
        master = parameters["amp.out_master"].getFloat().get_value();
    }
    std::ostringstream out;
    out << "{\"type\":\"snapshot\",\"state\":{\"version\":11,\"preset\":";
    write_json_string(out, bank + "/" + preset);
    out << ",\"master_volume_db\":" << master << ",\"plugins\":[";
    bool first = true;
    auto append = [&](const std::vector<std::string>& units) {
        for (const std::string& id : units) {
            gx_engine::Plugin* plugin = engine.pluginlist.find_plugin(id);
            if (!plugin || !plugin->get_pdef()) continue;
            if (!first) out << ',';
            first = false;
            write_module(out, parameters, plugin);
        }
    };
    append(settings.get_rack_unit_order(false));
    append(settings.get_rack_unit_order(true));
    out << "]},\"catalog\":[]}";
    std::cout << out.str() << std::endl;
}

std::size_t field_position(const std::string& line, const std::string& key) {
    return line.find("\"" + key + "\":");
}

std::string string_field(const std::string& line, const std::string& key) {
    std::size_t position = field_position(line, key);
    if (position == std::string::npos) return {};
    position = line.find('"', position + key.size() + 3);
    if (position == std::string::npos) return {};
    const std::size_t end = line.find('"', position + 1);
    return end == std::string::npos ? std::string() : line.substr(position + 1, end - position - 1);
}

double number_field(const std::string& line, const std::string& key, bool& valid) {
    std::size_t position = field_position(line, key);
    if (position == std::string::npos) {
        valid = false;
        return 0.0;
    }
    position += key.size() + 3;
    try {
        std::size_t consumed = 0;
        const double value = std::stod(line.substr(position), &consumed);
        valid = consumed > 0 && std::isfinite(value);
        return value;
    } catch (...) {
        valid = false;
        return 0.0;
    }
}

bool bool_field(const std::string& line, const std::string& key, bool& valid) {
    std::size_t position = field_position(line, key);
    if (position == std::string::npos) {
        valid = false;
        return false;
    }
    position += key.size() + 3;
    if (line.compare(position, 4, "true") == 0) {
        valid = true;
        return true;
    }
    if (line.compare(position, 5, "false") == 0) {
        valid = true;
        return false;
    }
    valid = false;
    return false;
}

void send_error(const std::string& message) {
    std::ostringstream out;
    out << "{\"type\":\"error\",\"message\":";
    write_json_string(out, message);
    out << '}';
    std::cout << out.str() << std::endl;
}

void handle_command(const std::string& line, gx_engine::GxEngine& engine,
                     gx_preset::GxSettings& settings, std::string& bank,
                     std::string& preset, const std::function<void()>& mark_dirty,
                     const std::function<void()>& flush_dirty) {
    const std::string type = string_field(line, "type");
    gx_engine::ParamMap& parameters = engine.get_param();
    if (type == "get_state") {
        send_snapshot(engine, settings, bank, preset);
        return;
    }
    if (type == "set_master_volume") {
        bool valid = false;
        const double value = number_field(line, "value", valid);
        if (!valid || !parameters.hasId("amp.out_master")) {
            send_error("invalid master volume");
            return;
        }
        parameters["amp.out_master"].getFloat().set(value);
        mark_dirty();
        return;
    }
    if (type == "set_preset") {
        const std::string requested_bank = string_field(line, "bank");
        const std::string requested_preset = string_field(line, "preset");
        gx_system::PresetFile *pf = settings.banks.get_file(requested_bank);
        if (!pf || !pf->has_entry(requested_preset)) {
            send_error("unknown Guitarix preset: " + requested_bank + "/" + requested_preset);
            return;
        }
        flush_dirty();
        settings.load_preset(pf, requested_preset);
        bank = requested_bank;
        preset = requested_preset;
        send_snapshot(engine, settings, bank, preset);
        return;
    }
    const std::string module_id = string_field(line, "id");
    gx_engine::Plugin* plugin = engine.pluginlist.find_plugin(module_id);
    if (!plugin) {
        send_error("unknown Guitarix module: " + module_id);
        return;
    }
    if (type == "set_bypass") {
        bool valid = false;
        const bool bypassed = bool_field(line, "bypassed", valid);
        if (!valid) {
            send_error("invalid bypass value");
            return;
        }
        plugin->set_on_off(!bypassed);
        mark_dirty();
        send_snapshot(engine, settings, bank, preset);
        return;
    }
    if (type == "set_parameter") {
        bool index_valid = false;
        bool value_valid = false;
        const double index_value = number_field(line, "index", index_valid);
        const double value = number_field(line, "value", value_valid);
        const auto controls = module_parameters(parameters, plugin);
        const std::size_t index = index_value < 0 ? controls.size()
            : static_cast<std::size_t>(index_value);
        if (!index_valid || !value_valid || index >= controls.size()) {
            send_error("invalid parameter update");
            return;
        }
        gx_engine::Parameter* parameter = controls[index];
        if (parameter->isFloat()) {
            parameter->getFloat().set(value);
        } else if (parameter->isInt()) {
            parameter->getInt().set(static_cast<int>(std::lround(value)));
        } else {
            parameter->getBool().set(value >= 0.5);
        }
        mark_dirty();
        return;
    }
    send_error("unsupported command: " + type);
}

} // namespace

static void usage(const char *argv0) {
    std::cerr << "usage: " << argv0
              << " [--bank NAME] [--preset NAME]"
              << " [--preset-dir DIR] [--plugin-dir DIR]"
              << " [--instance NAME]\n";
    std::exit(2);
}

int main(int argc, char **argv) {
    std::string bank = "my stuff";
    std::string preset = "main";
    std::string preset_dir = "";
    std::string plugin_dir = "";
    std::string instance = "guitar-rack-dsp";

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto need = [&](const char *name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << name << " needs a value\n";
                usage(argv[0]);
            }
            return argv[++i];
        };
        if (a == "--bank") {
            bank = need("--bank");
        } else if (a == "--preset") {
            preset = need("--preset");
        } else if (a == "--preset-dir") {
            preset_dir = need("--preset-dir");
        } else if (a == "--plugin-dir") {
            plugin_dir = need("--plugin-dir");
        } else if (a == "--instance") {
            instance = need("--instance");
        } else if (a == "-h" || a == "--help") {
            usage(argv[0]);
        } else {
            std::cerr << "unknown arg: " << a << "\n";
            usage(argv[0]);
        }
    }

    Glib::init();
    Gio::init();

    gx_system::CmdlineOptions options;
    if (!preset_dir.empty()) {
        options.set_preset_dir(preset_dir);
    }
    if (!plugin_dir.empty()) {
        options.set_plugin_dir(plugin_dir);
    }
    options.set_jack_instancename(instance);
    options.set_jack_noconnect(true);
    options.set_jack_single(true);
    options.set_bank_preset(bank, preset);

    bool need_new_preset = false;
    gx_preset::GxSettings::check_settings_dir(options, &need_new_preset);

    gx_engine::GxEngine engine(options.get_plugin_dir(),
                               gx_engine::get_group_table(), options);
    gx_jack::GxJack jack(engine);
    engine.oscilloscope.set_jack(jack);
    gx_preset::GxSettings settings(
        options, jack, engine.stereo_convolver, gx_engine::midi_std_ctr,
        engine.controller_map, engine);
    settings.disable_autosave(true);
    settings.disable_save_on_exit(true);

    gx_system::PresetFile *startup_preset = nullptr;
    if (need_new_preset) {
        settings.create_default_scratch_preset();
    } else {
        startup_preset = settings.banks.get_file(bank);
        if (!startup_preset || !startup_preset->has_entry(preset)) {
            std::cerr << "guitar-rack-dsp: bank '" << bank << "' or preset '"
                      << preset << "' not found in '" << options.get_preset_dir()
                      << "'\n";
            return 1;
        }
    }

    if (!jack.gx_jack_connection(true, true, 0, options)) {
        std::cerr << "guitar-rack-dsp: can't connect to jack\n";
        return 1;
    }
    if (startup_preset) {
        settings.load_preset(startup_preset, preset);
    }

    std::cerr << "guitar-rack-dsp: active, instance='" << instance
              << "' bank='" << bank << "' preset='" << preset << "'\n";
    std::cout << "guitar-rack-dsp-ready" << std::endl;

    engine.tuner.used_for_display(true);

    sigc::connection preset_save_timer;
    bool preset_dirty = false;
    std::string dirty_bank;
    std::string dirty_preset;
    auto save_dirty = [&]() {
        if (!preset_dirty) {
            return;
        }
        preset_dirty = false;
        gx_system::PresetFile *pf = settings.banks.get_file(dirty_bank);
        if (!pf || !pf->is_mutable() || !pf->has_entry(dirty_preset)) {
            send_error("cannot save Guitarix preset: " + dirty_bank + "/" + dirty_preset);
            return;
        }
        if (settings.get_current_bank() != dirty_bank
            || settings.get_current_name() != dirty_preset) {
            send_error("refusing stale Guitarix preset save");
            return;
        }
        settings.save(*pf, dirty_preset);
    };
    auto flush_dirty = [&]() {
        preset_save_timer.disconnect();
        save_dirty();
    };
    auto mark_dirty = [&]() {
        dirty_bank = bank;
        dirty_preset = preset;
        preset_dirty = true;
        preset_save_timer.disconnect();
        preset_save_timer = Glib::signal_timeout().connect([&]() {
            save_dirty();
            return false;
        }, 500);
    };

    std::mutex command_mutex;
    std::deque<std::string> commands;
    Glib::Dispatcher command_dispatcher;
    command_dispatcher.connect([&]() {
        std::deque<std::string> pending;
        {
            std::lock_guard<std::mutex> lock(command_mutex);
            pending.swap(commands);
        }
        for (const std::string& command : pending) {
            handle_command(command, engine, settings, bank, preset,
                           mark_dirty, flush_dirty);
        }
    });
    std::thread([&]() {
        std::string line;
        while (std::getline(std::cin, line)) {
            {
                std::lock_guard<std::mutex> lock(command_mutex);
                commands.push_back(line);
            }
            command_dispatcher.emit();
        }
    }).detach();

    send_snapshot(engine, settings, bank, preset);
    float last_tuner_frequency = -1.0f;
    const auto tuner_connection = Glib::signal_timeout().connect([&]() {
        const float frequency = engine.tuner.get_freq();
        if (std::isfinite(frequency) && std::abs(frequency - last_tuner_frequency) >= 0.01f) {
            last_tuner_frequency = frequency;
            std::cout << "{\"type\":\"tuner\",\"frequency\":" << frequency << "}"
                      << std::endl;
        }
        return true;
    }, 50);

    Glib::RefPtr<Glib::MainLoop> loop = Glib::MainLoop::create();
    jack.shutdown.connect(sigc::mem_fun(loop.operator->(), &Glib::MainLoop::quit));
    loop->run();
    flush_dirty();
    return 0;
}
