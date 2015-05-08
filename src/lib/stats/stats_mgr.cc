// Copyright (C) 2015 Internet Systems Consortium, Inc. ("ISC")
//
// Permission to use, copy, modify, and/or distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND ISC DISCLAIMS ALL WARRANTIES WITH
// REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY
// AND FITNESS.  IN NO EVENT SHALL ISC BE LIABLE FOR ANY SPECIAL, DIRECT,
// INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM
// LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE
// OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
// PERFORMANCE OF THIS SOFTWARE.

#include <exceptions/exceptions.h>
#include <stats/stats_mgr.h>

using namespace std;

namespace isc {
namespace stats {

StatsMgr& StatsMgr::instance() {
    static StatsMgr stats_mgr;
    return (stats_mgr);
}

StatsMgr::StatsMgr()
    :global_(new StatContext()) {

}

void StatsMgr::setValue(const std::string& name, uint64_t value) {
    setValueInternal(name, value);
}

void StatsMgr::setValue(const std::string& name, double value) {
    setValueInternal(name, value);
}

void StatsMgr::setValue(const std::string& name, StatsDuration value) {
    setValueInternal(name, value);
}
void StatsMgr::setValue(const std::string& name, const std::string& value) {
    setValueInternal(name, value);
}

void StatsMgr::addValue(const std::string& name, uint64_t value) {
    addValueInternal(name, value);
}

void StatsMgr::addValue(const std::string& name, double value) {
    addValueInternal(name, value);
}

void StatsMgr::addValue(const std::string& name, StatsDuration value) {
    addValueInternal(name, value);
}

void StatsMgr::addValue(const std::string& name, const std::string& value) {
    addValueInternal(name, value);
}

ObservationPtr StatsMgr::getObservation(const std::string& name) const {
    /// @todo: Implement contexts.
    // Currently we keep everyting is a global context.
    return (global_->get(name));
}

void StatsMgr::addObservation(const ObservationPtr& stat) {
    /// @todo: Implement contexts.
    // Currently we keep everyting is a global context.
    return (global_->add(stat));
}

bool StatsMgr::deleteObservation(const std::string& name) {
    /// @todo: Implement contexts.
    // Currently we keep everyting is a global context.
    return (global_->del(name));
}

void StatsMgr::setMaxSampleAge(const std::string& ,
                               boost::posix_time::time_duration) {
    isc_throw(NotImplemented, "setMaxSampleAge not implemented");
}

void StatsMgr::setMaxSampleCount(const std::string& , uint32_t){
    isc_throw(NotImplemented, "setMaxSampleCount not implemented");
}

bool StatsMgr::reset(const std::string& name) {
    ObservationPtr obs = getObservation(name);
    if (obs) {
        obs->reset();
        return (true);
    } else {
        return (false);
    }
}

bool StatsMgr::del(const std::string& name) {
    return (global_->del(name));
}

void StatsMgr::removeAll() {
    global_->stats_.clear();
}

isc::data::ConstElementPtr StatsMgr::get(const std::string& name) const {
    isc::data::ElementPtr response = isc::data::Element::createMap(); // a map
    ObservationPtr obs = getObservation(name);
    if (obs) {
        response->set(name, obs->getJSON()); // that contains the observation
    }
    return (response);
}

isc::data::ConstElementPtr StatsMgr::getAll() const {
    isc::data::ElementPtr map = isc::data::Element::createMap(); // a map

    // Let's iterate over all stored statistics...
    for (std::map<std::string, ObservationPtr>::iterator s = global_->stats_.begin();
         s != global_->stats_.end(); ++s) {

        // ... and add each of them to the map.
        map->set(s->first, s->second->getJSON());
    }
    return (map);
}

void StatsMgr::resetAll() {
    // Let's iterate over all stored statistics...
    for (std::map<std::string, ObservationPtr>::iterator s = global_->stats_.begin();
         s != global_->stats_.end(); ++s) {

        // ... and reset each statistic.
        s->second->reset();
    }
}

size_t StatsMgr::count() const {
    return (global_->stats_.size());
}



};
};
