/* -*- C++ -*-
 * Cppcheck - A tool for static C/C++ code analysis
 * Copyright (C) 2007-2026 Cppcheck team.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef GUARD_PROGRAMMEMORY_H
#define GUARD_PROGRAMMEMORY_H

#include "config.h"
#include "mathlib.h"
#include "vfvalue.h" // needed for alias

#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class Scope;
class Token;
class Settings;

// Class used to handle heterogeneous lookup in unordered_map(since we can't use C++20 yet)
struct ExprIdToken {
    const Token* tok = nullptr;
    nonneg int exprid = 0;

    // cppcheck-suppress noExplicitConstructor
    // NOLINTNEXTLINE(google-explicit-constructor)
    ExprIdToken(const Token* tok);

    nonneg int getExpressionId() const;

    bool operator==(const ExprIdToken& rhs) const {
        return getExpressionId() == rhs.getExpressionId();
    }

    bool operator<(const ExprIdToken& rhs) const {
        return getExpressionId() < rhs.getExpressionId();
    }

    template<class T, class U>
    friend bool operator!=(const T& lhs, const U& rhs)
    {
        return !(lhs == rhs);
    }

    template<class T, class U>
    friend bool operator<=(const T& lhs, const U& rhs)
    {
        return !(lhs > rhs);
    }

    template<class T, class U>
    friend bool operator>(const T& lhs, const U& rhs)
    {
        return rhs < lhs;
    }

    template<class T, class U>
    friend bool operator>=(const T& lhs, const U& rhs)
    {
        return !(lhs < rhs);
    }

    const Token& operator*() const noexcept {
        return *tok;
    }

    const Token* operator->() const noexcept {
        return tok;
    }

    struct Hash {
        std::size_t operator()(ExprIdToken etok) const;
    };

    /** create object for hashed lookups */
    static ExprIdToken create(nonneg int exprId) {
        return ExprIdToken(exprId);
    }

private:
    // for hashed lookups only
    explicit ExprIdToken(nonneg int exprId);
};

struct CPPCHECKLIB ProgramMemory {
    using Map = std::map<ExprIdToken, ValueFlow::Value>;

    ProgramMemory() : mValues(new Map()) {}

    explicit ProgramMemory(Map values) : mValues(new Map(std::move(values))) {}

    void setValue(const Token* expr, const ValueFlow::Value& value);
    const ValueFlow::Value* getValue(nonneg int exprid, bool impossible = false) const;

    bool getIntValue(nonneg int exprid, MathLib::bigint& result) const;
    void setIntValue(const Token* expr, MathLib::bigint value, bool impossible = false);

    bool getContainerSizeValue(nonneg int exprid, MathLib::bigint& result) const;
    bool getContainerEmptyValue(nonneg int exprid, MathLib::bigint& result) const;
    void setContainerSizeValue(const Token* expr, MathLib::bigint value, bool equal = true);

    void setUnknown(const Token* expr);

    bool getTokValue(nonneg int exprid, const Token*& result) const;
    bool hasValue(nonneg int exprid) const;

    const ValueFlow::Value& at(nonneg int exprid) const;
    ValueFlow::Value& at(nonneg int exprid);

    void erase_if(const std::function<bool(const ExprIdToken&)>& pred);

    void swap(ProgramMemory &pm) noexcept;

    void clear();

    bool empty() const;

    void replace(ProgramMemory pm, bool skipUnknown = false);

    Map::const_iterator begin() const {
        return mValues->cbegin();
    }

    Map::const_iterator end() const {
        return mValues->cend();
    }

    friend bool operator==(const ProgramMemory& x, const ProgramMemory& y) {
        return x.mValues == y.mValues;
    }

    friend bool operator!=(const ProgramMemory& x, const ProgramMemory& y) {
        return x.mValues != y.mValues;
    }

private:
    void copyOnWrite();
    Map::const_iterator find(nonneg int exprid) const;
    Map::iterator find(nonneg int exprid);

    std::shared_ptr<Map> mValues;
};

struct ProgramMemoryState {
    struct ChangedKeyHash {
        std::size_t operator()(const std::pair<const Token*, const Token*>& t) const
        {
            const std::hash<const Token*> h;
            std::size_t seed = h(t.first);
            seed ^= h(t.second) + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
    // What is known about the changes of expr after start
    struct ChangedRange {
        // expr is not changed in [start, unchangedEnd)
        const Token* unchangedEnd{};
        // expr is changed by changedTok in [start, changedEnd)
        const Token* changedEnd{};
        const Token* changedTok{};
    };
    using ChangedCache = std::unordered_map<std::pair<const Token*, const Token*>, ChangedRange, ChangedKeyHash>;
    // A token modifying expr between start and end (not always the one findExpressionChanged()
    // returns), or nullptr.
    using FindChangedFn = std::function<const Token*(const Token* expr, const Token* start, const Token* end)>;

    // mutable for the addState() that is done when the state is used, see addStateLater()
    mutable ProgramMemory state;
    mutable std::map<nonneg int, const Token*> origins;
    const Settings& settings;
    // Memoized findExpressionChanged() pre-filter; structural, so never invalidated.
    std::shared_ptr<ChangedCache> changedCache;

    explicit ProgramMemoryState(const Settings& s);
    // the pending addState() of other is done first, so that the copies do not depend on when they
    // do it
    ProgramMemoryState(const ProgramMemoryState& other);

    void addState(const Token* tok, const ProgramMemory::Map& vars);

    // addState() when the state is used next, the program memory is not computed when it is not
    // used. addState() reads the values of the tokens up to tok (findExpressionChanged() reads their
    // lifetime values), so only lifetime values may be set on the tokens until then, and each one
    // must be announced with beforeSetLifetimeValue().
    void addStateLater(const Token* tok, const ProgramMemory::Map& vars);

    // A lifetime value is set on tok and possibly on its parents: the pending addState() is done
    // first when one of them is not after its token.
    void beforeSetLifetimeValue(const Token* tok) const;

    // In builds with assertions: assert that the state that addStateLater() computes later is the
    // state that addState() would have computed. It is slow, the testrunner turns it on.
    static bool compareDeferredStates;

    void assume(const Token* tok, bool b, bool isEmpty = false, const Token* origin = nullptr);

    void removeModifiedVars(const Token* tok);

    // A findExpressionChanged() closure memoized in changedCache
    FindChangedFn getCachedFindExpressionChanged(bool skipDeadCode) const;

    ProgramMemory get(const Token* tok, const Token* ctx, const ProgramMemory::Map& vars) const;

private:
    // Do the addState() of addStateLater()
    void addPendingState() const;
    // addState() without the pending addState(), it changes the mutable members
    void addStateNow(const Token* tok, const ProgramMemory::Map& vars) const;
    void replace(ProgramMemory pm, const Token* origin) const;

    mutable const Token* pendingTok{};
    mutable ProgramMemory::Map pendingVars;
    // can the pending addState() read lifetime values
    mutable bool pendingReadsLifetimeValues{};
#ifndef NDEBUG
    // the state and the origins that the pending addState() would have computed at once
    mutable std::shared_ptr<std::pair<ProgramMemory, std::map<nonneg int, const Token*>>> pendingCheck;
#endif
};

std::vector<ValueFlow::Value> execute(const Scope* scope, ProgramMemory& pm, const Settings& settings);

void execute(const Token* expr,
             ProgramMemory& programMemory,
             MathLib::bigint* result,
             bool* error,
             const Settings& settings,
             const ProgramMemory::Map& vars = {});

/**
 * Is condition always false when variable has given value?
 * \param condition   top ast token in condition
 * \param pm   program memory
 * \param vars  optional tracked values that take precedence over the program memory
 */
bool conditionIsFalse(const Token* condition,
                      ProgramMemory pm,
                      const Settings& settings,
                      const ProgramMemory::Map& vars = {});

/**
 * Is condition always true when variable has given value?
 * \param condition   top ast token in condition
 * \param pm   program memory
 * \param vars  optional tracked values that take precedence over the program memory
 */
bool conditionIsTrue(const Token* condition,
                     ProgramMemory pm,
                     const Settings& settings,
                     const ProgramMemory::Map& vars = {});

/**
 * Get program memory by looking backwards from given token.
 */
ProgramMemory getProgramMemory(const Token* tok, const Token* expr, const ValueFlow::Value& value, const Settings& settings);

ValueFlow::Value evaluateLibraryFunction(const std::unordered_map<nonneg int, ValueFlow::Value>& args,
                                         const std::string& returnValue,
                                         const Settings& settings,
                                         bool cpp);

#endif



