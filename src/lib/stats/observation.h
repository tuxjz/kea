// Copyright (C) 2015 Internet Systems Consortium, Inc. ("ISC")
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#ifndef OBSERVATION_H
#define OBSERVATION_H

#include <cc/data.h>
#include <exceptions/exceptions.h>
#include <boost/shared_ptr.hpp>
#include <boost/date_time/time_duration.hpp>
#include <boost/date_time/posix_time/posix_time_types.hpp>
#include <list>
#include <stdint.h>

namespace isc {
namespace stats {

/// @brief Exception thrown if invalid statistic type is used
///
/// For example statistic is of type duration, but methods using
/// it as integer are called.
class InvalidStatType : public Exception {
public:
    InvalidStatType(const char* file, size_t line, const char* what) :
        isc::Exception(file, line, what) {}
};

/// @brief Defines duration resolution
///
typedef boost::posix_time::time_duration StatsDuration;

/// @defgroup stat_samples Specifies supported observation types.
///
/// @brief The list covers all supported types of observations.
///
/// @{

/// @brief Integer (implemented as signed 64-bit integer)
typedef std::pair<int64_t, boost::posix_time::ptime> IntegerSample;

/// @brief Float (implemented as double precision)
typedef std::pair<double, boost::posix_time::ptime> FloatSample;

/// @brief Time Duration
typedef std::pair<StatsDuration, boost::posix_time::ptime> DurationSample;

/// @brief String
typedef std::pair<std::string, boost::posix_time::ptime> StringSample;

/// @}

/// @brief Represents a single observable characteristic (a 'statistic')
///
/// Currently it supports one of four types: integer (implemented as signed 64
/// bit integer), float (implemented as double), time duration (implemented with
/// millisecond precision) and string. Absolute (setValue) and
/// incremental (addValue) modes are supported. Statistic type is determined
/// during its first use. Once type is set, any additional observations recorded
/// must be of the same type. Attempting to set or extract information about
/// other types will result in InvalidStateType exception.
///
/// Observation can be retrieved in one of @ref getInteger, @ref getFloat,
/// @ref getDuration, @ref getString (appropriate type must be used) or
/// @ref getJSON, which is generic and can be used for all types.
///
/// @todo: Eventually it will be possible to retain multiple samples for the same
/// observation, but that is outside of scope for 0.9.2.
class Observation {
 public:

    /// @brief Type of available statistics
    ///
    /// Note that those will later be exposed using control socket. Therefore
    /// an easy to understand names were chosen (integer instead of uint64).
    /// To avoid confusion, we will support only one type of integer and only
    /// one type of floating points. Initially, these are represented by
    /// int64_t and double. If convincing use cases appear to change them
    /// to something else, we may change the underlying type.
    enum Type {
        STAT_INTEGER, ///< this statistic is unsigned 64-bit integer value
        STAT_FLOAT,   ///< this statistic is a floating point value
        STAT_DURATION,///< this statistic represents time duration
        STAT_STRING   ///< this statistic represents a string
    };

    /// @brief Constructor for integer observations
    ///
    /// @param name observation name
    /// @param value integer value observed.
    Observation(const std::string& name, const int64_t value);

    /// @brief Constructor for floating point observations
    ///
    /// @param name observation name
    /// @param value floating point value observed.
    Observation(const std::string& name, const double value);

    /// @brief Constructor for duration observations
    ///
    /// @param name observation name
    /// @param value duration observed.
    Observation(const std::string& name, const StatsDuration& value);

    /// @brief Constructor for string observations
    ///
    /// @param name observation name
    /// @param value string observed.
    Observation(const std::string& name, const std::string& value);

    /// @brief Determines maximum age of samples.
    ///
    /// Specifies that statistic name should be stored not as a single value,
    /// but rather as a set of values. The duration determines the timespan.
    /// Samples older than duration will be discarded. This is time-constrained
    /// approach. For sample count constrained approach, see @ref
    /// setMaxSampleCount() below.
    ///
    ///
    /// @param duration determines maximum age of samples
    /// Example:
    /// To set a statistic to keep observations for the last 5 minutes,
    /// call: setMaxSampleAge(time_duration(0, 5, 0, 0));
    /// To revert statistic to a single value, call:
    /// setMaxSampleAge(time_duration(0, 0, 0, 0))
    void setMaxSampleAge(const StatsDuration& duration);

    /// @brief Determines how many samples of a given statistic should be kept.
    ///
    /// Specifies that statistic name should be stored not as a single value,
    /// but rather as a set of values. In this form, at most max_samples will
    /// be kept. When adding max_samples + 1 sample, the oldest sample will be
    /// discarded.
    ///
    ///
    /// @param max_samples how many samples of a given statistic should be kept
    /// Example:
    /// To set a statistic to keep the last 100 observations, call:
    /// setMaxSampleCount(100);
    void setMaxSampleCount(uint32_t max_samples);

    /// @brief Records absolute integer observation
    ///
    /// @param value integer value observed
    /// @throw InvalidStatType if statistic is not integer
    void setValue(const int64_t value);

    /// @brief Records absolute floating point observation
    ///
    /// @param value floating point value observed
    /// @throw InvalidStatType if statistic is not fp
    void setValue(const double value);

    /// @brief Records absolute duration observation
    ///
    /// @param value duration value observed
    /// @throw InvalidStatType if statistic is not time duration
    void setValue(const StatsDuration& value);

    /// @brief Records absolute string observation
    ///
    /// @param value string value observed
    /// @throw InvalidStatType if statistic is not a string
    void setValue(const std::string& value);

    /// @brief Records incremental integer observation
    ///
    /// @param value integer value observed
    /// @throw InvalidStatType if statistic is not integer
    void addValue(const int64_t value);

    /// @brief Records incremental floating point observation
    ///
    /// @param value floating point value observed
    /// @throw InvalidStatType if statistic is not fp
    void addValue(const double value);

    /// @brief Records incremental duration observation
    ///
    /// @param value duration value observed
    /// @throw InvalidStatType if statistic is not time duration
    void addValue(const StatsDuration& value);

    /// @brief Records incremental string observation.
    ///
    /// @param value string value observed
    /// @throw InvalidStatType if statistic is not a string
    void addValue(const std::string& value);

    /// @brief Returns size of observed storage
    ///
    /// @return size of storage
    size_t getSize() const;

    /// @brief Resets statistic.
    ///
    /// Sets statistic to a neutral (0, 0.0 or "") value.
    void reset();

    /// @brief Returns statistic type
    /// @return statistic type
    Type getType() const {
        return (type_);
    }

    /// @brief Returns observed integer sample
    /// @return observed sample (value + timestamp)
    /// @throw InvalidStatType if statistic is not integer
    IntegerSample getInteger() const;

    /// @brief Returns observed float sample
    /// @return observed sample (value + timestamp)
    /// @throw InvalidStatType if statistic is not fp
    FloatSample getFloat() const;

    /// @brief Returns observed duration sample
    /// @return observed sample (value + timestamp)
    /// @throw InvalidStatType if statistic is not time duration
    DurationSample getDuration() const;

    /// @brief Returns observed string sample
    /// @return observed sample (value + timestamp)
    /// @throw InvalidStatType if statistic is not a string
    StringSample getString() const;

    /// @brief Returns observed integer samples
    /// @return list of observed samples (value + timestamp)
    /// @throw InvalidStatType if statistic is not integer
    std::list<IntegerSample> getIntegers() const;

    /// @brief Returns observed float samples
    /// @return list of observed samples (value + timestamp)
    /// @throw InvalidStatType if statistic is not fp
    std::list<FloatSample> getFloats() const;

    /// @brief Returns observed duration samples
    /// @return list of observed samples (value + timestamp)
    /// @throw InvalidStatType if statistic is not time duration
    std::list<DurationSample> getDurations() const;

    /// @brief Returns observed string samples
    /// @return list of observed samples (value + timestamp)
    /// @throw InvalidStatType if statistic is not a string
    std::list<StringSample> getStrings() const;

    /// @brief Returns as a JSON structure
    /// @return JSON structures representing all observations
    isc::data::ConstElementPtr getJSON() const;

    /// @brief Converts statistic type to string
    /// @return textual name of statistic type
    static std::string typeToText(Type type);

    /// @brief Returns observation name
    std::string getName() const {
        return (name_);
    }

private:

    /// @brief Returns size of observed storage
    ///
    /// This method returns size of observed storage.
    /// It is used by public methods to return size of
    /// available storages.
    /// @tparam Storage type of storage (e.g. list<IntegerSample>)
    /// @param storage storage which size will be returned
    /// @param exp_type expected observation type (used for sanity checking)
    /// @return Size of storage
    template<typename StorageType>
    size_t getSizeInternal(StorageType& storage, Type exp_type) const;

    /// @brief Records absolute sample (internal version)
    ///
    /// This method records an absolute value of an observation.
    /// It is used by public methods to add sample to one of
    /// available storages.
    ///
    /// @tparam SampleType type of sample (e.g. IntegerSample)
    /// @tparam StorageType type of storage (e.g. list<IntegerSample>)
    /// @param value observation to be recorded
    /// @param storage observation will be stored here
    /// @param exp_type expected observation type (used for sanity checking)
    /// @throw InvalidStatType if observation type mismatches
    template<typename SampleType, typename StorageType>
    void setValueInternal(SampleType value, StorageType& storage,
                          Type exp_type);

    /// @brief Returns a sample (internal version)
    ///
    /// @tparam SampleType type of sample (e.g. IntegerSample)
    /// @tparam StorageType type of storage (e.g. list<IntegerSample>)
    /// @param observation storage
    /// @param exp_type expected observation type (used for sanity checking)
    /// @throw InvalidStatType if observation type mismatches
    /// @return Observed sample
    template<typename SampleType, typename Storage>
    SampleType getValueInternal(Storage& storage, Type exp_type) const;

    /// @brief Returns samples (internal version)
    ///
    /// @tparam SampleType type of samples (e.g. IntegerSample)
    /// @tparam Storage type of storage (e.g. list<IntegerSample>)
    /// @param observation storage
    /// @param exp_type expected observation type (used for sanity checking)
    /// @throw InvalidStatType if observation type mismatches
    /// @return List of observed samples
    template<typename SampleType, typename Storage>
    std::list<SampleType> getValuesInternal(Storage& storage,
        Type exp_type) const;

    /// @brief Determines maximum age of samples.
    ///
    /// @tparam Storage type of storage (e.g. list<IntegerSample>)
    /// @param storage storage on which limit will be set
    /// @param duration determines maximum age of samples
    /// @param exp_type expected observation type (used for sanity checking)
    template<typename StorageType>
    void setMaxSampleAgeInternal(StorageType& storage,
        const StatsDuration& duration, Type exp_type);

    /// @brief Determines how many samples of a given statistic should be kept.
    ///
    /// @tparam Storage type of storage (e.g. list<IntegerSample>)
    /// @param storage storage on which limit will be set
    /// @param max_samples determines maximum number of samples
    /// @param exp_type expected observation type (used for sanity checking)
    template<typename StorageType>
    void setMaxSampleCountInternal(StorageType& storage,
        uint32_t max_samples, Type exp_type);

    /// @brief Observation (statistic) name
    std::string name_;

    /// @brief Observation (statistic) type)
    Type type_;

    /// @brief Maximum number of samples
    /// The limit is represent as a pair
    /// of bool value and uint32_t
    /// Only one kind of limit can be active
    /// The bool value informs which limit
    /// is available
    /// True means active limit, false means inactive limit
    /// By default the MaxSampleCount is set to 20
    /// and MaxSampleAge is disabled
    std::pair<bool, uint32_t> max_sample_count = std::make_pair(true, 20);

    /// @brief Maximum timespan of samples
    /// The limit is represent as a pair
    /// of bool value and StatsDuration(boost::posix_time::time_duration)
    /// Only one kind of limit can be active
    /// The bool value informs which limit
    /// is available
    /// True means active limit, false means inactive limit
    /// By default the MaxSampleCount is set to 20
    /// and MaxSampleAge is disabled
    std::pair<bool, StatsDuration> max_sample_age = std::make_pair(false,
            boost::posix_time::time_duration(0, 0, 0, 0));

    /// @defgroup samples_storage Storage for supported observations
    ///
    /// @brief The following containers serve as a storage for all supported
    /// observation types.
    ///
    /// @{

    /// @brief Storage for integer samples
    std::list<IntegerSample> integer_samples_;

    /// @brief Storage for floating point samples
    std::list<FloatSample> float_samples_;

    /// @brief Storage for time duration samples
    std::list<DurationSample> duration_samples_;

    /// @brief Storage for string samples
    std::list<StringSample> string_samples_;
    /// @}
};

/// @brief Observation pointer
typedef boost::shared_ptr<Observation> ObservationPtr;

};
};

#endif // OBSERVATION_H
