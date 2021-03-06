// lug - Embedded DSL for PE grammar parser combinators in C++
// Copyright (c) 2017 Jesse W. Towner
// See LICENSE.md file for license details

#include <lug/lug.hpp>
#include <cassert>

namespace
{
	void test_any_terminal()
	{
		using namespace lug::language;
		rule S = any > eoi;
		grammar G = start(S);
		assert(lug::parse("a", G));
		assert(lug::parse("2", G));
		assert(lug::parse("z", G));
		assert(!lug::parse("aa", G));
		assert(!lug::parse("", G));
	}

	void test_empty_terminal()
	{
		using namespace lug::language;
		rule S = eps > eoi;
		grammar G = start(S);
		assert(lug::parse("", G));
		assert(!lug::parse("a", G));
		assert(!lug::parse("2", G));
		assert(!lug::parse("z", G));
		assert(!lug::parse("aa", G));
	}

	void test_char_terminal()
	{
		using namespace lug::language;
		rule S = chr('a') > eoi;
		grammar G = start(S);
		assert(lug::parse("a", G));
		assert(!lug::parse("", G));
		assert(!lug::parse("2", G));
		assert(!lug::parse("aa", G));
		assert(!lug::parse("b", G));
	}

	void test_terminal_sequence()
	{
		using namespace lug::language;
		rule S = chr('a') > any > chr('b') > eoi;
		grammar G = start(S);
		assert(lug::parse("a2b", G));
		assert(lug::parse("azb", G));
		assert(!lug::parse("", G));
		assert(!lug::parse("a", G));
		assert(!lug::parse("aza", G));
		assert(!lug::parse("azb3", G));
	}

	void test_terminal_choice()
	{
		using namespace lug::language;
		rule S = (chr('a') | chr('b')) > eoi;
		grammar G = start(S);
		assert(lug::parse("a", G));
		assert(lug::parse("b", G));
		assert(!lug::parse("", G));
		assert(!lug::parse("ab", G));
		assert(!lug::parse("ba", G));
	}

	void test_terminal_zero_or_one()
	{
		using namespace lug::language;
		rule S = ~chr('x') > eoi;
		grammar G = start(S);
		assert(lug::parse("", G));
		assert(lug::parse("x", G));
		assert(!lug::parse("xx", G));
		assert(!lug::parse("xxxxxxx", G));
		assert(!lug::parse("y", G));
		assert(!lug::parse("xy", G));
		assert(!lug::parse("xxxxxy", G));
	}

	void test_terminal_zero_or_many()
	{
		using namespace lug::language;
		rule S = *chr('x') > eoi;
		grammar G = start(S);
		assert(lug::parse("", G));
		assert(lug::parse("x", G));
		assert(lug::parse("xx", G));
		assert(lug::parse("xxxxxxx", G));
		assert(!lug::parse("y", G));
		assert(!lug::parse("xy", G));
		assert(!lug::parse("xxxxxy", G));
	}

	void test_terminal_one_or_many()
	{
		using namespace lug::language;
		rule S = +chr('x') > eoi;
		grammar G = start(S);
		assert(lug::parse("x", G));
		assert(lug::parse("xx", G));
		assert(lug::parse("xxxxxxx", G));
		assert(!lug::parse("", G));
		assert(!lug::parse("y", G));
		assert(!lug::parse("xy", G));
		assert(!lug::parse("xxxxxy", G));
	}

	void test_terminal_not()
	{
		using namespace lug::language;
		rule S = !chr('x') > any > eoi;
		grammar G = start(S);
		assert(lug::parse("y", G));
		assert(lug::parse("Z", G));
		assert(lug::parse("2", G));
		assert(!lug::parse("", G));
		assert(!lug::parse("x", G));
		assert(!lug::parse("xx", G));
		assert(!lug::parse("y2", G));
		assert(!lug::parse("yx", G));
	}

	void test_terminal_predicate()
	{
		using namespace lug::language;
		rule S = &chr('x') > any > any > eoi;
		grammar G = start(S);
		assert(lug::parse("xx", G));
		assert(lug::parse("xy", G));
		assert(lug::parse("xZ", G));
		assert(lug::parse("x2", G));
		assert(!lug::parse("", G));
		assert(!lug::parse("y", G));
		assert(!lug::parse("Z", G));
		assert(!lug::parse("2", G));
		assert(!lug::parse("x", G));
		assert(!lug::parse("y2", G));
	}
}

int main()
{
	try {
		test_any_terminal();
		test_empty_terminal();
		test_char_terminal();
		test_terminal_sequence();
		test_terminal_choice();
		test_terminal_zero_or_one();
		test_terminal_zero_or_many();
		test_terminal_one_or_many();
		test_terminal_not();
		test_terminal_predicate();
	} catch (std::exception& e) {
		std::cerr << "Error: " << e.what() << std::endl;
		return -1;
	}
	return 0;
}
