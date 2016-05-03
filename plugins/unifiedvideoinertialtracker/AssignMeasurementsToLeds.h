/** @file
    @brief Header

    @date 2016

    @author
    Sensics, Inc.
    <http://sensics.com/osvr>
*/

// Copyright 2016 Sensics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//        http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef INCLUDED_AssignMeasurementsToLeds_h_GUID_F7146BCA_13BF_4C91_1EE6_27E9FF039AD7
#define INCLUDED_AssignMeasurementsToLeds_h_GUID_F7146BCA_13BF_4C91_1EE6_27E9FF039AD7

// Internal Includes
#include "BeaconIdTypes.h"
#include "LED.h"
#include <LedMeasurement.h>

// Library/third-party includes
#include <boost/assert.hpp>
#include <opencv2/core/core.hpp>

// Standard includes
#include <algorithm>
#include <cstddef>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace osvr {
namespace vbtracker {

    /// In theory this shouldn't happen, but there are checks
    /// scattered all over the code. Now we can say that it doesn't
    /// happen because we won't let any bad values escape this
    /// routine.
    inline bool handleOutOfRangeIds(Led &led, const std::size_t numBeacons) {
        if (led.identified() &&
            /// cast to unsigned safe since identified implies non-negative
            static_cast<std::size_t>(makeZeroBased(led.getID()).value()) >
                numBeacons) {
            std::cerr << "Got a beacon claiming to be "
                      << led.getOneBasedID().value() << " when we only have "
                      << numBeacons << " beacons" << std::endl;
            /// @todo a kinder way of doing this? Right now this blows away
            /// the measurement history
            led.markMisidentified();
            return true;
        }
        return false;
    }

    /// Get the squared distance between two OpenCV points
    inline float sqDist(cv::Point2f const &lhs, cv::Point2f const &rhs) {
        auto diff = lhs - rhs;
        return diff.dot(diff);
    }

    class AssignMeasurementsToLeds {
        static const char *getPrefix() { return "[AssignMeasurements] "; }

      public:
        AssignMeasurementsToLeds(LedGroup &leds,
                                 LedMeasurementVec const &measurements,
                                 const std::size_t numBeacons,
                                 float blobMoveThresh)
            : leds_(leds), measurements_(measurements), ledsEnd_(end(leds_)),
              numBeacons_(numBeacons_), blobMoveThreshFactor_(blobMoveThresh) {}

        using LedAndMeasurement = std::pair<Led &, LedMeasurement const &>;

        using LedMeasDistance = std::tuple<std::size_t, std::size_t, float>;
        using HeapValueType = LedMeasDistance;
        using HeapType = std::vector<HeapValueType>;
        using size_type = HeapType::size_type;

        /// Must call first, and only once.
        void populateStructures() {
            BOOST_ASSERT_MSG(!populated_,
                             "Can only call populateStructures() once.");
            populated_ = true;
            {
                /// Clean up LEDs and populate their ref vector.
                auto led = begin(leds_);
                while (led != end(leds_)) {
                    led->resetUsed();
                    handleOutOfRangeIds(*led, numBeacons_);
                    ledRefs_.push_back(led);
                    ++led;
                }
            }
            for (auto &meas : measurements_) {
                /// Populate the measurement ref vector.
                measRefs_.push_back(&meas);
            }

            /// Do the O(n * m) distance computation to populate the vector that
            /// will become our min-heap.
            auto nMeas = measRefs_.size();
            auto nLed = ledRefs_.size();
            for (size_type measIdx = 0; measIdx < nMeas; ++measIdx) {
                auto distThreshSquared =
                    getDistanceThresholdSquared(*measRefs_[measIdx]);
                for (size_type ledIdx = 0; ledIdx < nLed; ++ledIdx) {
                    /// WARNING: watch the order of arguments to this function,
                    /// since the type of the indices is identical...
                    possiblyPushLedMeasurement(ledIdx, measIdx,
                                               distThreshSquared);
                }
            }
            /// Turn that vector into our min-heap.
            makeHeap();
        }

        /// Discards invalid entries (those where either the LED or the
        /// measurement, or both, have already been assigned) from the heap, and
        /// returns the count of entries so discarded.
        size_type discardInvalidEntries(bool verbose = false) {
            checkAndThrowNotPopulated("discardInvalidEntries()");
            size_type discarded = 0;
            if (empty()) {
                return discarded;
            }

            while (!empty()) {
                if (verbose) {
                    auto top = distanceHeap_.front();
                    std::cout << getPrefix() << "top: led index "
                              << ledIndex(top) << "\tmeas index "
                              << measIndex(top) << "\tsq dist "
                              << squaredDistance(top);
                    auto ledValid = (ledRefs_[ledIndex(top)] != ledsEnd_);
                    auto measValid = (measRefs_[measIndex(top)] != nullptr);
                    if (ledValid && measValid) {
                        std::cout << " both valid: ";
                    } else if (ledValid) {
                        std::cout << " only LED  valid: ";
                    } else if (measValid) {
                        std::cout << " only measurement valid: ";
                    } else {
                        std::cout << " neither valid: ";
                    }
                }
                if (isTopValid()) {
                    if (verbose) {
                        std::cout << "isTopValid() says keep!\n";
                    }
                    /// Great, we found one!
                    return discarded;
                }
                if (verbose) {
                    std::cout << "isTopValid() says discard!\n";
                }
                popHeap();
                discarded++;
            }
            return discarded;
        }

        /// In case a measurement update goes bad, we can try to "un-mark" a
        /// measurement as consumed.
        bool resumbitMeasurement(LedMeasurement const &meas) {
            auto it = std::find(begin(measurements_), end(measurements_), meas);
            if (it == end(measurements_)) {
                // sorry, can't help...
                return false;
            }
            auto idx = std::distance(begin(measurements_), it);
            if (measRefs_[idx] != nullptr) {
                std::cerr << "Trying to resubmit, but the measurement wasn't "
                             "marked as consumed!"
                          << std::endl;
                return false;
            }
            measRefs_[idx] = &(*it);
            return true;
        }

        /// Searches the heap, discarding now-invalid entries, until it finds an
        /// entry where both the LED and the measurement are unclaimed, or it
        /// runs out of entries.
        bool hasMoreMatches() {
            checkAndThrowNotPopulated("hasMoreMatches()");
            discardInvalidEntries();
            if (empty()) {
                return false;
            }
            return isTopValid();
        }

        /// Requires that hasMoreMatches() has been run and returns true.
        LedAndMeasurement getMatch(bool verbose = false) {
            checkAndThrowNotPopulated("getMatch()");
            auto hasMatch = hasMoreMatches();
            if (!hasMatch) {
                throw std::logic_error("Can't call getMatch() without first "
                                       "getting success from hasMoreMatches()");
            }
            if (verbose) {
                std::cout << getPrefix() << "Led Index "
                          << ledIndex(distanceHeap_.front()) << "\tMeas Index "
                          << measIndex(distanceHeap_.front()) << std::endl;
            }
            auto &topLed = *getTopLed();
            auto &topMeas = *getTopMeasurement();
            /// Mark that we've used this LED and measurement.
            markTopConsumed();
            /// Now, remove this entry from the heap.
            popHeap();
            /// and return the reward.
            return LedAndMeasurement(topLed, topMeas);
        }

        bool empty() const {
            /// Not terribly harmful here, just illogical, so assert instead of
            /// unconditional check and throw.
            BOOST_ASSERT_MSG(populated_, "Must have called "
                                         "populateStructures() before calling "
                                         "empty().");
            return distanceHeap_.empty();
        }

        /// Entries in the heap.
        size_type size() const {
            /// Not terribly harmful here, just illogical, so assert instead of
            /// unconditional check and throw.
            BOOST_ASSERT_MSG(
                populated_,
                "Must have called populateStructures() before calling size().");
            return distanceHeap_.size();
        }

        /// This is the size it could have potentially been, had all LEDs been
        /// within the distance threshold. (O(n m))
        size_type theoreticalMaxSize() const {
            return leds_.size() * measurements_.size();
        }

        /// The fraction of the theoretical max that the size is.
        double heapSizeFraction() const {
            /// Not terribly harmful here, just illogical, so assert instead of
            /// unconditional check and throw.
            BOOST_ASSERT_MSG(populated_, "Must have called "
                                         "populateStructures() before calling "
                                         "heapSizeFraction().");
            return static_cast<double>(size()) /
                   static_cast<double>(theoreticalMaxSize());
        }

        size_type numUnclaimedLedObjects() const {
            return std::count_if(
                begin(ledRefs_), end(ledRefs_),
                [&](LedIter const &it) { return it != ledsEnd_; });
        }

        void eraseUnclaimedLedObjects(bool verbose = false) {
            for (auto &ledIter : ledRefs_) {
                if (ledIter == ledsEnd_) {
                    /// already used
                    continue;
                }
                if (verbose) {
                    if (ledIter->identified()) {
                        std::cout << "Erasing identified LED "
                                  << ledIter->getOneBasedID().value()
                                  << " because of a lack of updated data.\n";
                    } else {
                        std::cout << "Erasing unidentified LED at "
                                  << ledIter->getLocation()
                                  << " because of a lack of updated data.\n";
                    }
                }
                leds_.erase(ledIter);
            }
        }

        size_type numUnclaimedMeasurements() const {
            return std::count_if(
                begin(measRefs_), end(measRefs_),
                [&](MeasPtr const &ptr) { return ptr != nullptr; });
        }

        template <typename F> void forEachUnclaimedMeasurement(F &&op) {
            for (auto &measRef : measRefs_) {
                if (!measRef) {
                    /// already used
                    continue;
                }
                /// Apply the operation.
                std::forward<F>(op)(*measRef);
            }
        }

      private:
        using LedIter = LedGroup::iterator;
        using LedPtr = Led *;
        using MeasPtr = LedMeasurement const *;
        void checkAndThrowNotPopulated(const char *functionName) const {
            if (!populated_) {
                throw std::logic_error(
                    "Must have called populateStructures() before calling " +
                    std::string(functionName));
            }
        }

        /// @name Accessors for tuple elements.
        /// @{
        static std::size_t ledIndex(LedMeasDistance const &val) {
            return std::get<0>(val);
        }
        static std::size_t &ledIndex(LedMeasDistance &val) {
            return std::get<0>(val);
        }
        static std::size_t measIndex(LedMeasDistance const &val) {
            return std::get<1>(val);
        }
        static std::size_t &measIndex(LedMeasDistance &val) {
            return std::get<1>(val);
        }
        static float squaredDistance(LedMeasDistance const &val) {
            return std::get<2>(val);
        }
        /// @}

        void possiblyPushLedMeasurement(std::size_t ledIdx, std::size_t measIdx,
                                        float distThreshSquared) {
            auto meas = measRefs_[measIdx];
            auto led = ledRefs_[ledIdx];
            auto squaredDist = sqDist(led->getLocation(), meas->loc);
            if (squaredDist < distThreshSquared) {
                // If we're within the threshold, let's push this candidate on
                // the vector that will be turned into a heap.
                distanceHeap_.emplace_back(ledIdx, measIdx, distThreshSquared);
            }
        }
        LedIter getTopLed() const {
            return ledRefs_[ledIndex(distanceHeap_.front())];
        }
#if 0
        LedPtr getTopLedPtr() const {
            auto ledRef = getTopLed();
            if (ledRef == getEmptyLed()) {
                return nullptr;
            }
            return &(*ledRef);
        }
#endif
        MeasPtr getTopMeasurement() const {
            return measRefs_[measIndex(distanceHeap_.front())];
        }

        bool isTopValid() const {
            LedMeasDistance elt = distanceHeap_.front();
            return (ledRefs_[ledIndex(elt)] != ledsEnd_) &&
                   measRefs_[measIndex(elt)];
        }

        void markTopConsumed() {
            LedMeasDistance elt = distanceHeap_.front();
            ledRefs_[ledIndex(elt)] = ledsEnd_;
            measRefs_[measIndex(elt)] = nullptr;
        }

        float getDistanceThresholdSquared(LedMeasurement const &meas) const {
            auto thresh = blobMoveThreshFactor_ * meas.diameter;
            return thresh * thresh;
        }

        /// min heap comparator needs greater-than, want to compare on third
        /// tuple element.
        class Comparator {
          public:
            bool operator()(HeapValueType const &lhs,
                            HeapValueType const &rhs) const {
                return squaredDistance(lhs) > squaredDistance(rhs);
            }
        };

        void makeHeap() {
            /// cost of 3 * len, which is O(n m)
            std::make_heap(begin(distanceHeap_), end(distanceHeap_),
                           Comparator());
        }
        class HeapUsage {
          public:
            HeapUsage(HeapType &heap) : heap_(heap), n_(heap.size()) {}
            ~HeapUsage() {
                if (numPopped_ > 0) {
                    heap_.resize(n_ - numPopped_);
                }
            }
            HeapUsage(HeapUsage const &) = delete;
            HeapUsage &operator=(HeapUsage const &) = delete;
            using size_type = HeapType::size_type;
            void pop() {
                if (empty()) {
                    return;
                }

                std::pop_heap(begin(heap_), end(heap_), Comparator());
                numPopped_++;
            }

            bool empty() const { return n_ == 0 || n_ == numPopped_; }

            size_type size() const { return n_ - numPopped_; }

          private:
            HeapType &heap_;
            const HeapType::size_type n_;
            std::size_t numPopped_ = 0;
        };

        /// does not resize for you!
        void rawPopHeap() {
            BOOST_ASSERT_MSG(!distanceHeap_.empty(),
                             "Cannot pop from an empty heap");
            std::pop_heap(begin(distanceHeap_), end(distanceHeap_),
                          Comparator());
        }

        /// Resizes for you too.
        void popHeap() {
            BOOST_ASSERT_MSG(!distanceHeap_.empty(),
                             "Cannot pop from an empty heap");
            std::pop_heap(begin(distanceHeap_), end(distanceHeap_),
                          Comparator());
            distanceHeap_.pop_back();
        }

        void dropLastEntries(size_type numEntries) {
            if (numEntries == 0) {
                return;
            }
            auto n = distanceHeap_.size();
            BOOST_ASSERT_MSG(
                numEntries <= n,
                "Cannot drop more entries from heap than exist in it.");
            distanceHeap_.resize(n - numEntries);
        }

        LedGroup &leds_;
        LedMeasurementVec const &measurements_;
        const LedIter ledsEnd_;
        const std::size_t numBeacons_;
        const float blobMoveThreshFactor_;
        bool populated_ = false;
        std::vector<LedIter> ledRefs_;
        std::vector<MeasPtr> measRefs_;
        HeapType distanceHeap_;
    };

} // namespace vbtracker
} // namespace osvr

#endif // INCLUDED_AssignMeasurementsToLeds_h_GUID_F7146BCA_13BF_4C91_1EE6_27E9FF039AD7
