/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 * Modified by ScyllaDB
 * Copyright (C) 2015 ScyllaDB
 */

/*
 * This file is part of Scylla.
 *
 * Scylla is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Scylla is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Scylla.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#include <unordered_map>
#include <utility>
#include <experimental/optional>
#include <unordered_set>
#include <boost/filesystem.hpp>

#include "gms/endpoint_state.hh"
#include "gms/gossiper.hh"
#include "utils/fb_utilities.hh"
#include "locator/token_metadata.hh"
#include "db/system_keyspace.hh"
#include "db/config.hh"
#include "core/sstring.hh"
#include "snitch_base.hh"

namespace locator {

class bad_property_file_error : public std::exception {};

class production_snitch_base : public snitch_base {
public:
    // map of inet address to (datacenter, rack) pair
    typedef std::unordered_map<inet_address, endpoint_dc_rack> addr2dc_rack_map;

    static constexpr const char* default_dc   = "UNKNOWN_DC";
    static constexpr const char* default_rack = "UNKNOWN_RACK";
    static constexpr const char* snitch_properties_filename = "cassandra-rackdc.properties";

    // only these property values are supported
    static constexpr const char* dc_property_key           = "dc";
    static constexpr const char* rack_property_key         = "rack";
    static constexpr const char* prefer_local_property_key = "prefer_local";
    static constexpr const char* dc_suffix_property_key    = "dc_suffix";
    const std::unordered_set<sstring> allowed_property_keys;

    production_snitch_base(const sstring& prop_file_name = "")
    : allowed_property_keys({ dc_property_key,
                              rack_property_key,
                              prefer_local_property_key,
                              dc_suffix_property_key }){
        if (!prop_file_name.empty()) {
            _prop_file_name = prop_file_name;
        } else {
            using namespace boost::filesystem;

            path def_prop_file(db::config::get_conf_dir());
            def_prop_file /= path(snitch_properties_filename);

            _prop_file_name = def_prop_file.string();
        }
    }

    virtual sstring get_rack(inet_address endpoint) {
        if (endpoint == utils::fb_utilities::get_broadcast_address()) {
            return _my_rack;
        }

        return get_endpoint_info(endpoint,
                                 gms::application_state::RACK,
                                 default_rack);
    }

    virtual sstring get_datacenter(inet_address endpoint) {
        if (endpoint == utils::fb_utilities::get_broadcast_address()) {
            return _my_dc;
        }

        return get_endpoint_info(endpoint,
                                 gms::application_state::DC,
                                 default_dc);
    }

    virtual void set_my_distributed(distributed<snitch_ptr>* d) override {
        _my_distributed = d;
    }

    void reset_io_state() {
        //
        // Reset the promise to allow repeating
        // start()+stop()/pause_io()+resume_io() call sequences.
        //
        _io_is_stopped = promise<>();
    }

private:
    sstring get_endpoint_info(inet_address endpoint, gms::application_state key,
                              const sstring& default_val) {
        gms::gossiper& local_gossiper = gms::get_local_gossiper();
        auto state = local_gossiper.get_endpoint_state_for_endpoint(endpoint);

        // First, look in the gossiper::endpoint_state_map...
        if (state) {
            auto ep_state = state->get_application_state(key);
            if (ep_state) {
                return ep_state->value;
            }
        }

        // ...if not found - look in the SystemTable...
        if (!_saved_endpoints) {
            _saved_endpoints = db::system_keyspace::load_dc_rack_info();
        }

        auto it = _saved_endpoints->find(endpoint);

        if (it != _saved_endpoints->end()) {
            if (key == gms::application_state::RACK) {
                return it->second.rack;
            } else { // gms::application_state::DC
                return it->second.dc;
            }
        }

        // ...if still not found - return a default value
        return default_val;
    }

    virtual void set_my_dc(const sstring& new_dc) override {
        _my_dc = new_dc;
    }

    virtual void set_my_rack(const sstring& new_rack) override {
        _my_rack = new_rack;
    }

    virtual void set_prefer_local(bool prefer_local) override {
        _prefer_local = prefer_local;
    }

    void parse_property_file();

protected:
    /**
     * Loads the contents of the property file into the map
     *
     * @return ready future when the file contents has been loaded.
     */
    future<> load_property_file();

    void throw_double_declaration(const sstring& key) const {
        logger().error("double \"{}\" declaration in {}", key, _prop_file_name);
        throw bad_property_file_error();
    }

    void throw_bad_format(const sstring& line) const {
        logger().error("Bad format in properties file {}: {}", _prop_file_name, line);
        throw bad_property_file_error();
    }

    void throw_incomplete_file() const {
        logger().error("Property file {} is incomplete. Some obligatory fields are missing.", _prop_file_name);
        throw bad_property_file_error();
    }

protected:
    promise<> _io_is_stopped;
    std::experimental::optional<addr2dc_rack_map> _saved_endpoints;
    distributed<snitch_ptr>* _my_distributed = nullptr;
    std::string _prop_file_contents;
    sstring _prop_file_name;
    std::unordered_map<sstring, sstring> _prop_values;

private:
    size_t _prop_file_size;
};
} // namespace locator
