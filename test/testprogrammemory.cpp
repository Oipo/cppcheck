/*
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

#include "config.h"
#include "fixture.h"
#include "helpers.h"
#include "token.h"
#include "programmemory.h"
#include "utils.h"
#include "vfvalue.h"

#include <initializer_list>
#include <stdexcept>

class TestProgramMemory : public TestFixture {
public:
    TestProgramMemory() : TestFixture("TestProgramMemory") {}

private:
    void run() override {
        TEST_CASE(copyOnWrite);
        TEST_CASE(hasValue);
        TEST_CASE(getValue);
        TEST_CASE(at);
        TEST_CASE(addStateLater);
    }

    void copyOnWrite() const {
        SimpleTokenList tokenlist("1+1;\n");
        Token* tok = tokenlist.front();
        const nonneg int id = 123;
        tok->exprId(id);

        ProgramMemory pm;
        const ValueFlow::Value* v = pm.getValue(id);
        ASSERT(!v);
        pm.setValue(tok, ValueFlow::Value{41});

        v = pm.getValue(id);
        ASSERT(v);
        ASSERT_EQUALS(41, v->intvalue);

        // create a copy
        ProgramMemory pm2 = pm;

        // make sure the value was copied
        v = pm2.getValue(id);
        ASSERT(v);
        ASSERT_EQUALS(41, v->intvalue);

        // set a value in the copy to trigger copy-on-write
        pm2.setValue(tok, ValueFlow::Value{42});

        // make another copy and set another value
        ProgramMemory pm3 = pm2;

        // set a value in the copy to trigger copy-on-write
        pm3.setValue(tok, ValueFlow::Value{43});

        // make sure the value was set
        v = pm2.getValue(id);
        ASSERT(v);
        ASSERT_EQUALS(42, v->intvalue);

        // make sure the value was set
        v = pm3.getValue(id);
        ASSERT(v);
        ASSERT_EQUALS(43, v->intvalue);

        // make sure the original value remains unchanged
        v = pm.getValue(id);
        ASSERT(v);
        ASSERT_EQUALS(41, v->intvalue);
    }

    void hasValue() const {
        ProgramMemory pm;
        ASSERT(!pm.hasValue(123));
    }

    void getValue() const {
        ProgramMemory pm;
        ASSERT(!pm.getValue(123));
    }

    void at() const {
        ProgramMemory pm;
        ASSERT_THROW_EQUALS(pm.at(123), std::out_of_range, "ProgramMemory::at");
        ASSERT_THROW_EQUALS(utils::as_const(pm).at(123), std::out_of_range, "ProgramMemory::at");
    }

    static bool isSameState(const ProgramMemory& pm1, const ProgramMemory& pm2) {
        auto it2 = pm2.begin();
        for (auto it1 = pm1.begin(); it1 != pm1.end(); ++it1, ++it2) {
            if (it2 == pm2.end() || it1->first.getExpressionId() != it2->first.getExpressionId() || !(it1->second == it2->second))
                return false;
        }
        return it2 == pm2.end();
    }

    void addStateLater() {
        const char code[] = "void f(int c) {\n"
                            "    int x = 1;\n"
                            "    int y = x;\n"
                            "    if (c) {\n"
                            "        x = 2;\n"
                            "        g(x, 1);\n"
                            "    }\n"
                            "}\n";
        SimpleTokenizer tokenizer(settingsDefault, *this);
        ASSERT(tokenizer.tokenize(code));
        const Token* const x2 = Token::findsimplematch(tokenizer.tokens(), "x = 2");
        const Token* const call = Token::findsimplematch(x2, "g (");
        const Token* const arg = call->tokAt(2);
        const Token* const one = call->tokAt(4);
        const Token* const y = Token::findsimplematch(tokenizer.tokens(), "y = x");
        const Token* const end = Token::findsimplematch(call, "}");
        ASSERT(x2 && call && y && end);
        ASSERT_EQUALS("x", arg->str());
        ASSERT_EQUALS("1", one->str());
        const nonneg int x = x2->exprId();

        ProgramMemoryState now(settingsDefault);
        now.addState(arg, {});
        ASSERT(now.state.hasValue(x));
        ASSERT(now.state.hasValue(Token::findsimplematch(tokenizer.tokens(), "if ( c")->tokAt(2)->exprId()));

        // the state is computed when it is used, a copy uses it
        {
            ProgramMemoryState later(settingsDefault);
            later.addStateLater(arg, {});
            ASSERT(later.state.empty());
            const ProgramMemoryState copy(later);
            ASSERT(isSameState(now.state, copy.state));
            ASSERT(isSameState(now.state, later.state));
            ASSERT(now.origins == later.origins);
        }

        // a lifetime value that is set after the token, on a token and parents after it, does not
        // change the state
        {
            ProgramMemoryState later(settingsDefault);
            later.addStateLater(arg, {});
            later.beforeSetLifetimeValue(end);
            ASSERT(later.state.empty());
            later.beforeSetLifetimeValue(call->next()->link());
            ASSERT(later.state.empty());
            later.addState(end, {});
            ASSERT(!later.state.empty());
        }

        // a lifetime value on the token, before it, or on a token with a parent before it, can
        // change the state, which is computed first
        for (const Token* tok : { arg, x2, call, one }) {
            ProgramMemoryState later(settingsDefault);
            later.addStateLater(arg, {});
            later.beforeSetLifetimeValue(tok);
            ASSERT_EQUALS_MSG(false, later.state.empty(), tok->str());
            ASSERT(isSameState(now.state, later.state));
        }

        // without an enclosing condition, the state does not read lifetime values
        {
            ProgramMemoryState later(settingsDefault);
            later.addStateLater(y, {});
            later.beforeSetLifetimeValue(tokenizer.tokens());
            later.beforeSetLifetimeValue(y);
            ASSERT(later.state.empty());
            ProgramMemoryState ref(settingsDefault);
            ref.addState(y, {});
            const ProgramMemoryState copy(later);
            ASSERT(isSameState(ref.state, copy.state));
        }
    }
};

REGISTER_TEST(TestProgramMemory)
