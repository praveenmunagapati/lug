// lug - Embedded DSL for PE grammar parser combinators in C++
// Copyright (c) 2017 Jesse W. Towner
// See LICENSE.md file for license details

#ifndef LUG_HPP__
#define LUG_HPP__

#include <lug/detail.hpp>
#include <lug/error.hpp>
#include <lug/unicode.hpp>
#include <lug/utf8.hpp>
#include <any>
#include <iostream>
#include <numeric>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace lug
{

struct program; class rule; class grammar; class encoder; class rule_encoder; class parser; class semantics;
struct syntax_position { std::size_t column, line; };
struct syntax_range { std::size_t index, size; syntax_position start, end; };
struct syntax_view { std::string_view capture; syntax_position start, end; };
typedef std::function<bool(parser&)> semantic_predicate;
typedef std::function<void(semantics&)> semantic_action;
typedef std::function<void(semantics&, const syntax_view&)> semantic_capture;
struct semantic_response { unsigned short call_depth, action_index; unsigned int capture_index; };
template <class E> constexpr bool is_callable_v = std::is_same_v<grammar, std::decay_t<E>> || std::is_same_v<rule, std::decay_t<E>> || std::is_same_v<program, std::decay_t<E>>;
template <class E> constexpr bool is_predicate_v = std::is_invocable_r_v<bool, E, parser&> || std::is_invocable_r_v<bool, E>;
template <class E> constexpr bool is_proper_expression_v = std::is_invocable_v<E, encoder&>;
template <class E> constexpr bool is_string_expression_v = std::is_convertible_v<E, std::string>;
template <class E> constexpr bool is_expression_v = is_callable_v<E> || is_predicate_v<E> || is_proper_expression_v<E> || is_string_expression_v<E>;

enum class opcode : unsigned char
{
	match,          match_any,      match_class,    match_range,
	match_set,      choice,         commit,         jump,
	call,           ret,            fail,           accept,
	predicate,      action,         begin_capture,  end_capture
};

enum class altcode : unsigned char
{
	none = 0, accept_final = 1, commit_back = 1, commit_partial = 2,
	match_ptype = 1, match_gctype = 2, match_sctype = 3, match_scxtype = 4
};

enum class immediate : unsigned short {};
LUG_BITFIELD_ENUM__(operands, unsigned char) { none = 0, altcode = 0x3f, off = 0x40, str = 0x80 };
constexpr operands to_operands(altcode alt) noexcept { return static_cast<operands>(alt) & operands::altcode; }

union instruction
{
	static constexpr std::size_t maxstrlen = 256;
	struct prefix { opcode op; operands aux; unsigned short val; } pf;
	int off;
	std::array<char, 4> str;

	instruction(opcode op, operands aux, immediate imm) : pf{op, aux, static_cast<unsigned short>(imm)} {}
	instruction(std::ptrdiff_t o) : off{static_cast<int>(o)} { if (off != o) throw program_limit_error{}; }
	instruction(std::string_view s) { std::fill(std::copy_n(s.begin(), (std::min)(s.size(), std::size_t{4}), str.begin()), str.end(), char{0}); }

	static auto decode(const std::vector<instruction>& code, std::ptrdiff_t& pc) {
		auto pf = code[pc++].pf;
		auto imm = pf.val;
		auto off = (pf.aux & operands::off) != operands::none ? code[pc++].off : 0;
		std::string_view str;
		if ((pf.aux & operands::str) != operands::none) {
			str = std::string_view{code[pc].str.data(), static_cast<unsigned int>((imm & 0xff) + 1)};
			pc += ((imm & 0xff) + 4) >> 2;
			imm = (imm >> 8) + 1;
		}
		return std::make_tuple(pf.op, static_cast<altcode>(pf.aux & operands::altcode), imm, off, str);
	}

	static std::ptrdiff_t length(prefix pf) noexcept {
		std::ptrdiff_t len = 1;
		if ((pf.aux & operands::off) != operands::none)
			++len;
		if ((pf.aux & operands::str) != operands::none)
			len += static_cast<std::ptrdiff_t>(((pf.val & 0xff) >> 2) + 1);
		return len;
	}
};

static_assert(sizeof(instruction) == sizeof(int), "expected instruction to be same size as int");
static_assert(sizeof(int) <= sizeof(std::ptrdiff_t), "expected int to be no larger than ptrdiff_t");
static_assert(sizeof(unicode::ctype) <= sizeof(immediate), "immediate must be large enough to hold unicode::ctype");
static_assert(sizeof(unicode::sctype) <= sizeof(immediate), "immediate must be large enough to hold unicode::sctype");
LUG_BITFIELD_ENUM__(directives, unsigned int) { none = 0, caseless = 1, lexeme = 2, noskip = 4, matcheps = 8, predrain = 16, postdrain = 32 };

struct program
{
	std::vector<instruction> instructions;
	std::vector<semantic_predicate> predicates;
	std::vector<semantic_action> actions;
	std::vector<semantic_capture> captures;
	directives mandate{directives::matcheps};

	void concatenate(const program& src) {
		instructions.reserve(detail::checked_add<program_limit_error>(instructions.size(), src.instructions.size()));
		for (auto i = src.instructions.begin(), j = i, e = src.instructions.end(); i != e; i = j) {
			instruction instr = *i;
			std::size_t valoffset;
			switch (instr.pf.op) {
				case opcode::predicate: valoffset = predicates.size(); break;
				case opcode::action: valoffset = actions.size(); break;
				case opcode::end_capture: valoffset = captures.size(); break;
				default: valoffset = 0; break;
			}
			if (valoffset != 0) {
				std::size_t val = instr.pf.val + valoffset;
				detail::assure_in_range<resource_limit_error>(val, valoffset, (std::numeric_limits<unsigned short>::max)());
				instr.pf.val = static_cast<unsigned short>(val);
			}
			j = std::next(i, instruction::length(instr.pf));
			instructions.push_back(instr);
			instructions.insert(instructions.end(), i + 1, j);
		}
		predicates.insert(predicates.end(), src.predicates.begin(), src.predicates.end());
		actions.insert(actions.end(), src.actions.begin(), src.actions.end());
		captures.insert(captures.end(), src.captures.begin(), src.captures.end());
		mandate = (mandate & ~directives::matcheps) | (mandate & src.mandate & directives::matcheps);
	}

	void swap(program& p) {
		instructions.swap(p.instructions);
		predicates.swap(p.predicates);
		actions.swap(p.actions);
		captures.swap(p.captures);
		std::swap(mandate, p.mandate);
	}
};

class rule
{
	friend class encoder; friend class rule_encoder;
	friend grammar start(const rule&);
	lug::program program_;
	std::vector<std::tuple<const lug::rule*, const lug::program*, std::ptrdiff_t, directives>> callees_;
	bool currently_encoding_{false};
public:
	rule() = default;
	template <class E, class = std::enable_if_t<is_expression_v<E>>> rule(const E& e);
	rule(const rule& r);
	rule(rule&& r) = default;
	rule& operator=(const rule& r) { rule{r}.swap(*this); return *this; }
	rule& operator=(rule&& r) = default;
	void swap(rule& r) { program_.swap(r.program_); callees_.swap(r.callees_); }
	auto operator[](unsigned short precedence) const noexcept;
};

class grammar
{
	friend grammar start(const rule&);
	lug::program program_;
	grammar(lug::program p) : program_{std::move(p)} {}
public:
	grammar() = default;
	void swap(grammar& g) { program_.swap(g.program_); }
	const lug::program& program() const noexcept { return program_; };
	static thread_local std::function<void(encoder&)> space;
};

class semantics
{
	friend class parser;
	std::string_view match_;
	unsigned short prune_depth_{(std::numeric_limits<unsigned short>::max)()}, call_depth_{0};
	std::vector<std::any> attributes_;
	std::vector<std::pair<std::size_t, std::size_t>> captures_;
	std::vector<semantic_response> responses_;
	virtual void on_accept_begin() {}
	virtual void on_accept_end() {}
	virtual void on_clear() {}

	void pop_responses_after(std::size_t n) {
		if (n < responses_.size())
			responses_.resize(n);
	}

	auto drop_responses_after(std::size_t n) {
		std::vector<semantic_response> dropped;
		if (n < responses_.size()) {
			dropped.assign(responses_.begin() + n, responses_.end());
			responses_.resize(n);
		}
		return dropped;
	}

	auto restore_responses_after(std::size_t n, const std::vector<semantic_response>& restore) {
		pop_responses_after(n);
		responses_.insert(responses_.end(), restore.begin(), restore.end());
		return responses_.size();
	}

	auto push_response(std::size_t depth, std::size_t action_index, unsigned int capture_index = (std::numeric_limits<unsigned int>::max)()) {
		responses_.push_back({static_cast<unsigned short>(depth), static_cast<unsigned short>(action_index), capture_index});
		return responses_.size();
	}

	auto push_capture_response(std::size_t depth, std::size_t action_index, std::size_t index, std::size_t size) {
		captures_.emplace_back(index, size); // TODO: erase this on pop/drop? 
		return push_response(depth, action_index, static_cast<unsigned int>(captures_.size() - 1));
	}

public:
	virtual ~semantics() = default;
	const std::string_view& match() const { return match_; }
	void escape() { prune_depth_ = call_depth_; }
	unsigned short call_depth() const { return call_depth_; }
	template <class T> void push_attribute(T&& x) { attributes_.emplace_back(std::in_place_type<T>, ::std::forward<T>(x)); }
	template <class T, class... Args> void push_attribute(Args&&... args) { attributes_.emplace_back(std::in_place_type<T>, ::std::forward<Args>(args)...); }
	template <class T> T pop_attribute() { T r{::std::any_cast<T>(detail::pop_back(attributes_))}; return r; }

	void accept(const grammar& grmr, std::string_view m) {
		const auto& [actions, captures] = std::forward_as_tuple(grmr.program().actions, grmr.program().captures);
		match_ = m;
		on_accept_begin();
		for (auto& response : responses_) {
			if (prune_depth_ <= response.call_depth)
				continue;
			prune_depth_ = (std::numeric_limits<unsigned short>::max)(), call_depth_ = response.call_depth;
			if (response.capture_index < (std::numeric_limits<unsigned int>::max)()) {
				const auto& cap = captures_[response.capture_index];
				captures[response.action_index](*this, {match_.substr(cap.first, cap.second), syntax_position{}, syntax_position{}}); // TODO
			} else {
				actions[response.action_index](*this);
			}
		}
		on_accept_end();
		clear();
	}

	void clear() {
		match_ = std::string_view{}, prune_depth_ = (std::numeric_limits<unsigned short>::max)(), call_depth_ = 0;
		attributes_.clear(), captures_.clear(), responses_.clear();
		on_clear();
	}
};

template <class T>
class variable
{
	semantics& semantics_;
	std::unordered_map<unsigned short, T> state_;
public:
	variable(semantics& s) : semantics_{s} {}
	T* operator->() { return &state_[semantics_.call_depth()]; }
	const T* operator->() const { return &state_[semantics_.call_depth()]; }
	T& operator*() { return state_[semantics_.call_depth()]; }
	const T& operator*() const { return state_[semantics_.call_depth()]; }
};

class encoder
{
	virtual void do_append(instruction) = 0;
	virtual void do_append(const program&) = 0;
	virtual immediate do_add_semantic_predicate(semantic_predicate) { return immediate{0}; }
	virtual immediate do_add_semantic_action(semantic_action) { return immediate{0}; }
	virtual immediate do_add_semantic_capture_action(semantic_capture) { return immediate{0}; }
	virtual void do_add_callee(const rule*, const program*, std::ptrdiff_t, directives) {}
	virtual bool do_should_evaluate_length() const noexcept { return true; }
	virtual std::ptrdiff_t do_length() const noexcept = 0;
protected:
	std::vector<directives> mode_{directives::matcheps};
	encoder& add_callee(const rule* r, const program* p, std::ptrdiff_t n) {
		do_add_callee(r, p, n, detail::fetch_reduce_back(mode_, p->mandate & directives::matcheps, std::bit_and<>{}));
		return *this;
	}
public:
	virtual ~encoder() = default;
	encoder& dclr(directives clear, bool c = true) { if (c) { mode_.back() = mode_.back() & ~clear; } return *this; }
	encoder& dpop(directives relay) { directives prev = detail::pop_back(mode_); mode_.back() = (mode_.back() & ~relay) | (prev & relay); return *this; }
	encoder& dpsh(directives enable, directives disable) { directives prev = mode_.back(); mode_.push_back((prev & ~disable) | enable); return *this; }
	encoder& append(instruction instr) { do_append(instr); return *this; }
	encoder& append(const program& p) { do_append(p); return *this; }
	template <class InputIt> encoder& append(InputIt first, InputIt last) { for ( ; first != last; ++first) do_append(*first); return *this; }
	encoder& call(const program& p, unsigned short prec) { return add_callee(nullptr, &p, length()).encode(opcode::call, 0, immediate{prec}); }
	encoder& call(const grammar& g, unsigned short prec) { return add_callee(nullptr, &g.program(), length()).encode(opcode::call, 3, immediate{prec}); }
	encoder& encode(opcode op, immediate imm = immediate{0}) { return append(instruction{op, operands::none, imm}); }
	encoder& encode(opcode op, altcode alt, immediate imm = immediate{0}) { return append(instruction{op, to_operands(alt), imm}); }
	encoder& encode(opcode op, semantic_predicate p) { return append(instruction{op, operands::none, do_add_semantic_predicate(std::move(p))}); }
	encoder& encode(opcode op, semantic_action a) { return append(instruction{op, operands::none, do_add_semantic_action(std::move(a))}); }
	encoder& encode(opcode op, semantic_capture a) { return append(instruction{op, operands::none, do_add_semantic_capture_action(std::move(a))}); }
	encoder& encode(opcode op, std::ptrdiff_t off, immediate imm = immediate{0}) { return append(instruction{op, operands::off, imm}).append(instruction{off}); }
	encoder& encode(opcode op, std::size_t val, std::string_view subsequence) { return encode(op, altcode::none, val, subsequence); }
	template <class E> auto evaluate(const E& e) -> std::enable_if_t<is_expression_v<E>, encoder&>;
	template <class E> auto evaluate_length(const E& e) -> std::enable_if_t<is_expression_v<E>, std::ptrdiff_t>;
	std::ptrdiff_t length() const noexcept { return do_length(); }
	directives mode() const noexcept { return mode_.back(); }

	encoder& call(const rule& r, unsigned short prec, bool allow_inlining = true) {
		if (const auto& p = r.program_; allow_inlining && prec <= 0 && !r.currently_encoding_ && r.callees_.empty() && !p.instructions.empty() &&
					p.instructions.size() <= 8 && p.predicates.size() <= 1 && p.actions.size() <= 1 && p.captures.size() <= 1)
			return dclr(directives::matcheps, (p.mandate & directives::matcheps) == directives::none).append(p);
		return add_callee(&r, &r.program_, length()).encode(opcode::call, 0, immediate{prec});
	}

	encoder& encode(opcode op, altcode alt, std::ptrdiff_t off, immediate imm = immediate{0}) {
		return append(instruction{op, operands::off | to_operands(alt), imm}).append(instruction{off});
	}

	encoder& encode(opcode op, altcode alt, std::size_t val, std::string_view subsequence) {
		if (!subsequence.empty()) {
			detail::assure_in_range<resource_limit_error>(val, 1u, instruction::maxstrlen);
			detail::assure_in_range<resource_limit_error>(subsequence.size(), 1u, instruction::maxstrlen);
			do_append(instruction{op, operands::str | to_operands(alt), static_cast<immediate>(((val - 1) << 8) | (subsequence.size() - 1))});
			do {
				do_append(instruction{subsequence});
				subsequence.remove_prefix((std::min)(std::size_t{4}, subsequence.size()));
			} while (!subsequence.empty());
		}
		return *this;
	}

	encoder& match(std::string_view sequence) {
		while (sequence.size() > instruction::maxstrlen) {
			std::string_view subsequence = sequence.substr(0, instruction::maxstrlen);
			while (!subsequence.empty() && !utf8::is_lead(subsequence.back()))
				subsequence.remove_suffix(1);
			subsequence.remove_suffix(!subsequence.empty());
			encode(opcode::match, utf8::count_runes(subsequence.cbegin(), subsequence.cend()), subsequence);
			sequence.remove_prefix(subsequence.size());
		}
		return encode(opcode::match, utf8::count_runes(sequence.cbegin(), sequence.cend()), sequence).dclr(directives::matcheps, !sequence.empty());
	}

	encoder& match(std::string_view f, std::string_view l) { return f == l ? match(f) : encode(opcode::match_range, f.size(), std::string{f}.append(l)).dclr(directives::matcheps); }
	encoder& match(unicode::ctype flags) { return encode(opcode::match_class, immediate{static_cast<unsigned short>(flags)}).dclr(directives::matcheps); }
	encoder& match(unicode::ptype flags) { return encode(opcode::match_class, altcode::match_ptype, 1, detail::string_pack(flags)).dclr(directives::matcheps); }
	encoder& match(unicode::gctype flags) { return encode(opcode::match_class, altcode::match_gctype, 1, detail::string_pack(flags)).dclr(directives::matcheps); }
	encoder& match(unicode::sctype type) { return encode(opcode::match_class, altcode::match_sctype, immediate{static_cast<unsigned short>(type)}).dclr(directives::matcheps); }
};

class instruction_length_evaluator final : public encoder
{
	std::ptrdiff_t length_{0};
	void do_append(instruction) override { length_ = detail::checked_add<program_limit_error>(length_, std::ptrdiff_t{1}); }
	void do_append(const program& p) override { length_ = detail::checked_add<program_limit_error>(length_, static_cast<std::ptrdiff_t>(p.instructions.size())); }
	bool do_should_evaluate_length() const noexcept override { return false; }
	std::ptrdiff_t do_length() const noexcept override { return length_; }
};

class instruction_encoder : public encoder
{
	std::vector<instruction>& instructions_;
	std::ptrdiff_t do_length() const noexcept override final { return static_cast<std::ptrdiff_t>(instructions_.size()); }
	void do_append(instruction instr) override final { instructions_.push_back(instr); }
	void do_append(const program&) override { throw bad_grammar{}; }
public:
	explicit instruction_encoder(std::vector<instruction>& i) : instructions_{i} {}
};

class program_encoder : public instruction_encoder
{
	program& program_;
	void do_append(const program& p) override final { program_.concatenate(p); }
	immediate do_add_semantic_predicate(semantic_predicate p) override final { return add_item(program_.predicates, std::move(p)); }
	immediate do_add_semantic_action(semantic_action a) override final { return add_item(program_.actions, std::move(a)); }
	immediate do_add_semantic_capture_action(semantic_capture a) override final { return add_item(program_.captures, std::move(a)); }

	template <class Item>
	immediate add_item(std::vector<Item>& items, Item&& item) {
		detail::assure_in_range<resource_limit_error>(items.size(), 0u, (std::numeric_limits<unsigned short>::max)() - 1u);
		items.push_back(::std::forward<Item>(item));
		return static_cast<immediate>(items.size() - 1);
	}

public:
	explicit program_encoder(program& p) : instruction_encoder{p.instructions}, program_{p} {}
	~program_encoder() { program_.mandate = mode(); }
};

class rule_encoder final : public program_encoder
{
	rule& rule_;
	void do_add_callee(const rule* r, const program* p, std::ptrdiff_t n, directives d) override { rule_.callees_.emplace_back(r, p, n, d); }
public:
	explicit rule_encoder(rule& r) : program_encoder{r.program_}, rule_{r} { rule_.currently_encoding_ = true; }
	~rule_encoder() override { rule_.currently_encoding_ = false; }
};

class string_expression
{
	std::vector<instruction> instructions_;
	bool matches_eps_{true};
	static grammar make_grammar();
	void compile(std::string_view sv);

	struct generator : semantics
	{
		string_expression& owner;
		instruction_encoder encoder;
		std::vector<std::pair<std::string_view, std::string_view>> ranges;
		unicode::ctype classes = unicode::ctype::none;
		bool circumflex = false;

		generator(string_expression& se) : owner{se}, encoder{se.instructions_} {}
		~generator() { owner.matches_eps_ = (encoder.mode() & directives::matcheps) != directives::none; }

		void bracket_class(std::string_view s) {
			if (auto c = unicode::stoctype(s); c.has_value())
				classes |= c.value();
			throw bad_character_class{};
		}

		void bracket_range(std::string_view s) {
			bracket_range(s.substr(0, s.find('-')), s.substr(s.find('-') + 1));
		}

		void bracket_range(std::string_view first, std::string_view last) {
			ranges.emplace_back(first > last ? last : first, first > last ? first : last);
			std::push_heap(ranges.begin(), ranges.end(), [](auto& a, auto& b) { return a.first < b.first; });
		}

		void bracket_commit() {
			std::vector<instruction> matches;
			if (!ranges.empty()) {
				std::vector<std::pair<std::string_view, std::string_view>> merged;
				std::sort_heap(ranges.begin(), ranges.end(), [](auto& a, auto& b) { return a.first < b.first; });
				for (auto curr = merged.end(), next = ranges.begin(), last = ranges.end(); next != last; ++next)
					if (curr == merged.end() || next->first < curr->first || curr->second < next->first)
						curr = merged.insert(merged.end(), *next); 
					else
						curr->second = curr->second < next->second ? next->second : curr->second;
				if (auto curr = merged.crbegin(), last = merged.crend(); curr != last) {
					instruction_encoder{matches}.match(curr->first, curr->second);
					for (++curr; curr != last; ++curr) {
						std::vector<instruction> left, both;
						instruction_encoder{left}.match(curr->first, curr->second);
						instruction_encoder{both}
							.encode(opcode::choice, 2 + left.size()).append(left.begin(), left.end())
							.encode(opcode::commit, matches.size()).append(matches.begin(), matches.end());
						matches = std::move(both);
					}
				}
			}
			if (circumflex)
				encoder.encode(opcode::choice, 3 + matches.size() + (classes != unicode::ctype::none ? 1 : 0));
			encoder.append(matches.begin(), matches.end());
			if (classes != unicode::ctype::none)
				encoder.match(classes);
			if (circumflex)
				encoder.encode(opcode::commit, 0).encode(opcode::fail).encode(opcode::match_any);
			encoder.dclr(directives::matcheps);
			ranges.clear();
			classes = unicode::ctype::none;
			circumflex = false;
		}
	};

public:
	string_expression(std::string_view sv) { compile(sv); }
	void operator()(encoder& d) const { d.append(instructions_.cbegin(), instructions_.cend()).dclr(directives::matcheps, !matches_eps_); }
};

template <class T>
constexpr auto make_expression(const T& t) {
	static_assert(is_expression_v<T>, "T must be an expression type");
	if constexpr (is_callable_v<T>)
		return [&target = t](encoder& d){ d.call(target, 0); };
	else if constexpr (std::is_invocable_r_v<bool, T, parser&>)
		return [p = semantic_predicate{t}](encoder& d) { d.encode(opcode::predicate, p); };
	else if constexpr (std::is_invocable_r_v<bool, T>)
		return [p = semantic_predicate{[a = T{t}](parser&){ return a(); }}](encoder& d) { d.encode(opcode::predicate, p); };
	else if constexpr (is_string_expression_v<T>)
		return string_expression{t};
	else
		return t;
}

template <class E>
inline auto encoder::evaluate(const E& e) -> std::enable_if_t<is_expression_v<E>, encoder&> {
	make_expression(e)(*this);
	return *this;
}

template <class E>
inline auto encoder::evaluate_length(const E& e) -> std::enable_if_t<is_expression_v<E>, std::ptrdiff_t> {
	return do_should_evaluate_length() ? instruction_length_evaluator{}.evaluate(e).length() : 0;
}

template <class E, class> inline rule::rule(const E& e) { rule_encoder{*this}.evaluate(e); }
inline rule::rule(const rule& r) { rule_encoder{*this}.call(r, 1); }
inline auto rule::operator[](unsigned short precedence) const noexcept { return [&target = *this, precedence](encoder& d){ d.call(target, precedence); }; }

template <directives EnableMask, directives DisableMask, directives RelayMask>
struct directive_modifier {
	template <class E, class = std::enable_if_t<is_expression_v<E>>>
	auto operator[](const E& e) const { return [e = make_expression(e)](encoder& d) { d.dpsh(EnableMask, DisableMask).evaluate(e).dpop(RelayMask); }; }
};

constexpr auto pre_drain = directive_modifier<directives::predrain, directives::postdrain, directives::matcheps>{};
constexpr auto post_drain = directive_modifier<directives::postdrain, directives::none, directives::matcheps>{};
constexpr auto accept_eps = directive_modifier<directives::none, directives::none, directives::none>{};
constexpr auto relay_eps = directive_modifier<directives::none, directives::none, directives::matcheps | directives::predrain>{};

namespace language
{

using namespace std::literals::string_literals;
using lug::grammar; using lug::rule; using lug::start;
using parser = lug::parser; using semantics = lug::semantics; using syntax = const lug::syntax_view&;
template <class T> using variable = lug::variable<T>;

constexpr auto cased = directive_modifier<directives::none, directives::caseless, directives::matcheps>{};
constexpr auto caseless = directive_modifier<directives::caseless, directives::none, directives::matcheps>{};
constexpr auto lexeme = directive_modifier<directives::lexeme, directives::noskip, directives::matcheps>{};
constexpr auto no_skip = directive_modifier<directives::lexeme | directives::noskip, directives::none, directives::matcheps>{};
constexpr auto skip = directive_modifier<directives::none, directives::lexeme | directives::noskip, directives::matcheps>{};

constexpr struct { void operator()(encoder& d) const { d.encode(opcode::match_any).dclr(directives::matcheps); } } any = {};
constexpr struct { void operator()(encoder& d) const { d.encode(opcode::accept); } } cut = {};
constexpr struct { void operator()(encoder& d) const { d.encode(opcode::match); } } eps = {};
constexpr struct { void operator()(encoder& d) const { d.encode(opcode::choice, 2).encode(opcode::match_any).encode(opcode::fail, immediate{ 1 }); } } eoi = {};
constexpr struct { void operator()(encoder& d) const { d.match("\n"); } } eol = {}; // TODO: special rule for unicode line ending
constexpr struct { void operator()(encoder& d) const { d.match(unicode::ctype::space); } } space = {};

constexpr struct {
	auto operator()(char c) const { return [c](encoder& d) { d.match(std::string_view{&c, 1}); }; }
	auto operator()(char s, char e) const { return [s, e](encoder& d) { d.match(std::string_view{&s, 1}, std::string_view{&e, 1}); }; }
} chr = {};

template <class E, class = std::enable_if_t<is_expression_v<E>>>
constexpr auto operator!(const E& e) { return [x = make_expression(e)](encoder& d) {
		d.encode(opcode::choice, 1 + d.evaluate_length(x)).evaluate(accept_eps[x]).encode(opcode::fail, immediate{1}); }; }
template <class E, class = std::enable_if_t<is_expression_v<E>>>
constexpr auto operator&(const E& e) { return [x = make_expression(e)](encoder& d) {
		d.encode(opcode::choice, 2 + d.evaluate_length(x)).evaluate(accept_eps[x]).encode(opcode::commit, altcode::commit_back, 1).encode(opcode::fail); }; }
template <class E, class = std::enable_if_t<is_expression_v<E>>>
constexpr auto operator*(const E& e) { return [x = make_expression(e)](encoder& d) { auto n = d.evaluate_length(x);
		d.encode(opcode::choice, 2 + n).evaluate(accept_eps[post_drain[x]]).encode(opcode::commit, altcode::commit_partial, -(2 + n)); }; }
template <class E1, class E2, class = std::enable_if_t<is_expression_v<E1> && is_expression_v<E2>>>
constexpr auto operator|(const E1& e1, const E2& e2) { return [x1 = make_expression(e1), x2 = make_expression(e2)](encoder& d) {
		d.encode(opcode::choice, 2 + d.evaluate_length(x1)).evaluate(relay_eps[x1]).encode(opcode::commit, d.evaluate_length(x2)).evaluate(relay_eps[x2]); }; }
template <class E1, class E2, class = std::enable_if_t<is_expression_v<E1> && is_expression_v<E2>>>
constexpr auto operator>(const E1& e1, const E2& e2) { return [e1 = make_expression(e1), e2 = make_expression(e2)](encoder& d) { d.evaluate(e1).evaluate(pre_drain[e2]); }; }

template <class E, class A, class = std::enable_if_t<is_expression_v<E>>>
constexpr auto operator<(const E& e, A a) {
	if constexpr (std::is_invocable_v<A, semantics&, syntax>)
		return [e = make_expression(e), a = ::std::move(a)](encoder& d) { d.encode(opcode::begin_capture).evaluate(e).encode(opcode::end_capture, semantic_capture{a}); };
	else if constexpr (std::is_invocable_v<A, detail::dynamic_cast_if_base_of<semantics&>, syntax>)
		return e < [a = std::move(a)](semantics& s, syntax x) { a(detail::dynamic_cast_if_base_of<semantics&>{s}, x); };
	else if constexpr (std::is_invocable_v<A, semantics&>)
		return [e = make_expression(e), a = ::std::move(a)](encoder& d) { d.evaluate(e).encode(opcode::action, semantic_action{a}); };
	else if constexpr (std::is_invocable_v<A, detail::dynamic_cast_if_base_of<semantics&>>)
		return e < [a = std::move(a)](semantics& s) { a(detail::dynamic_cast_if_base_of<semantics&>{s}); };
	else if constexpr (std::is_invocable_v<A> && std::is_same_v<void, std::invoke_result_t<A>>)
		return [e = make_expression(e), a = ::std::move(a)](encoder& d) { d.evaluate(e).encode(opcode::action, [a](semantics&) { a(); }); };
	else if constexpr (std::is_invocable_v<A>)
		return [e = make_expression(e), a = ::std::move(a)](encoder& d) { d.evaluate(e).encode(opcode::action, [a](semantics& s) { s.push_attribute(a()); }); };
}

template <class E, class = std::enable_if_t<is_expression_v<E>>> constexpr auto operator+(const E& e) { auto x = make_expression(e); return x > *x; }
template <class E, class = std::enable_if_t<is_expression_v<E>>> constexpr auto operator~(const E& e) { return e | eps; }
template <class E, class = std::enable_if_t<is_expression_v<E>>> constexpr auto operator--(const E& e) { return cut > e; }
template <class E, class = std::enable_if_t<is_expression_v<E>>> constexpr auto operator--(const E& e, int) { return e > cut; }
template <class T, class E, class = std::enable_if_t<is_expression_v<E>>>
inline auto operator<<(variable<T>& v, const E& e) { return e < [&v](semantics&, syntax x) { *v = T{x.capture}; }; }
template <class T, class E, class = std::enable_if_t<is_expression_v<E>>>
inline auto operator%(variable<T>& v, const E& e) { return e < [&v](semantics& s) { *v = s.pop_attribute<T>(); }; }

} // namespace language

inline thread_local std::function<void(encoder&)> grammar::space{language::operator*(space)};

inline grammar start(const rule& start_rule) {
	program grprogram;
	std::unordered_map<const program*, std::ptrdiff_t> addresses;
	std::vector<std::pair<const program*, std::ptrdiff_t>> calls;
	std::unordered_set<const program*> left_recursive;
	std::vector<std::pair<std::vector<std::pair<const rule*, bool>>, const program*>> unprocessed;
	program_encoder{grprogram}.call(start_rule, 0, false).encode(opcode::accept, altcode::accept_final);
	calls.emplace_back(&start_rule.program_, 0);
	unprocessed.emplace_back(std::vector<std::pair<const rule*, bool>>{{&start_rule, false}}, &start_rule.program_);
	do {
		auto [callstack, subprogram] = detail::pop_back(unprocessed);
		const auto address = static_cast<std::ptrdiff_t>(grprogram.instructions.size());
		if (addresses.emplace(subprogram, address).second) {
			grprogram.concatenate(*subprogram);
			grprogram.instructions.emplace_back(opcode::ret, operands::none, immediate{0});
			if (auto top_rule = callstack.back().first; top_rule) {
				for (auto [callee_rule, callee_program, instr_offset, mode] : top_rule->callees_) {
					calls.emplace_back(callee_program, address + instr_offset);
					if (callee_rule && (mode & directives::matcheps) != directives::none && detail::escaping_find_if(
								callstack.crbegin(), callstack.crend(), [callee_rule](auto& caller) {
									return caller.first == callee_rule ? 1 : (caller.second ? 0 : -1); }) != callstack.crend()) {
						left_recursive.insert(callee_program);
					} else {
						auto callee_callstack = callstack;
						callee_callstack.emplace_back(callee_rule, (mode & directives::matcheps) != directives::none);
						unprocessed.emplace_back(std::move(callee_callstack), callee_program);
					}
				}
			}
		}
	} while (!unprocessed.empty());
	for (auto [subprogram, instr_addr] : calls) {
		if (auto& iprefix = grprogram.instructions[instr_addr]; iprefix.pf.op == opcode::call)
			iprefix.pf.val = left_recursive.count(subprogram) != 0 ? (iprefix.pf.val != 0 ? iprefix.pf.val : 1) : 0;
		auto& ioffset = grprogram.instructions[instr_addr + 1];
		auto rel_addr = ioffset.off + addresses[subprogram] - (instr_addr + 2);
		detail::assure_in_range<program_limit_error>(rel_addr, std::numeric_limits<int>::lowest(), (std::numeric_limits<int>::max)());
		ioffset.off = static_cast<int>(rel_addr);
	}
	return grammar{std::move(grprogram)};
}

struct parser_registers
{
	std::size_t sr, mr, rc; std::ptrdiff_t pc; std::size_t fc;
	auto as_tuple() noexcept { return std::forward_as_tuple(sr, mr, rc, pc, fc); }
	auto as_tuple() const noexcept { return std::forward_as_tuple(sr, mr, rc, pc, fc); }
};

class parser
{
	enum class stack_frame_type : unsigned char { backtrack, call, capture, lrcall };
	enum class subject : std::size_t {};
	struct lrmemo { std::size_t srr, sra, prec; std::ptrdiff_t pcr, pca; std::size_t rcr; std::vector<semantic_response> responses; };
	static constexpr std::size_t lrfailcode = (std::numeric_limits<std::size_t>::max)();
	const lug::grammar& grammar_;
	lug::semantics& semantics_;
	std::string input_;
	parser_registers registers_{0, 0, 0, 0, 0};
	bool parsing_{false}, reading_{false}, cut_deferred_{false};
	std::size_t cut_frame_{0};
	std::vector<std::function<bool(std::string&)>> sources_;
	std::vector<stack_frame_type> stack_frames_;
	std::vector<std::tuple<std::size_t, std::size_t, std::ptrdiff_t>> backtrack_stack_; // sr, rc, pc
	std::vector<std::ptrdiff_t> call_stack_; // pc
	std::vector<subject> capture_stack_; // sr
	std::vector<lrmemo> lrmemo_stack_;

	bool available(std::size_t n, std::size_t sr) {
		do {
			if (n <= input_.size() - sr)
				return true;
			if (sr < input_.size())
				return false;
		} while (read_more());
		return false;
	}

	bool read_more() {
		detail::reentrancy_sentinel<reenterant_read_error> guard{reading_};
		std::string text;
		while (!sources_.empty() && text.empty()) {
			bool more = sources_.back()(text);
			input_.insert(input_.end(), text.begin(), text.end());
			if (!more)
				sources_.pop_back();
		}
		return !text.empty();
	}

	void accept(std::size_t& sr, std::size_t& mr, std::size_t& rc, std::ptrdiff_t& pc) {
		registers_ = {sr, mr, rc, pc, 0};
		semantics_.accept(grammar_, input_);
		input_.erase(0, sr);
		registers_.sr = 0, registers_.mr, registers_.rc = 0;
		cut_deferred_ = false, cut_frame_ = stack_frames_.size();
		std::tie(sr, mr, rc, pc, std::ignore) = registers_.as_tuple();
	}

	template <class Stack, class... Args>
	void pop_stack_frame(Stack& stack, Args&... args) {
		stack.pop_back(), stack_frames_.pop_back();
		cut_frame_ = (std::min)(cut_frame_, stack_frames_.size());
		if constexpr (std::is_same_v<typename Stack::value_type, subject> || std::is_same_v<typename Stack::value_type, lrmemo>)
			if (cut_deferred_ && capture_stack_.empty() && lrmemo_stack_.empty())
				accept(args...);
	}

public:
	parser(const lug::grammar& g, lug::semantics& s) : grammar_{g}, semantics_{s} {}
	const lug::grammar& grammar() const noexcept { return grammar_; }
	lug::semantics& semantics() const noexcept { return semantics_; }
	std::string_view input_view() const noexcept { return {input_.data() + registers_.sr, input_.size() - registers_.sr}; }
	std::size_t input_position() const noexcept { return registers_.sr; }
	std::size_t max_input_position() const noexcept { return registers_.mr; }
	parser_registers& registers() noexcept { return registers_; }
	const parser_registers& registers() const noexcept { return registers_; }
	bool available(std::size_t n) { available(n, registers_.sr); }

	template <class InputIt, class = utf8::enable_if_char_input_iterator_t<InputIt>>
	parser& enqueue(InputIt first, InputIt last) {
		input_.insert(input_.end(), first, last);
		return *this;
	}

	template <class InputFunc>
	parser& push_source(InputFunc&& func) {
		if (reading_) throw reenterant_read_error{};
		sources_.emplace_back(::std::forward<InputFunc>(func));
		return *this;
	}

	bool parse() {
		detail::reentrancy_sentinel<reenterant_parse_error> guard{parsing_};
		const program& prog = grammar_.program();
		if (prog.instructions.empty()) throw bad_grammar{};
		auto [sr, mr, rc, pc, fc] = registers_;
		bool result = false, done = false;
		rc = 0, pc = 0, fc = 0, cut_deferred_ = false, cut_frame_ = 0;
		semantics_.clear();
		while (!done) {
			switch (auto [op, alt, imm, off, str] = instruction::decode(prog.instructions, pc); op) {
				case opcode::match: {
					if (!str.empty()) {
						if (!available(str.size(), sr) || input_.compare(sr, str.size(), str) != 0)
							goto failure;
						sr += str.size();
					}
				} break;
				case opcode::match_any: {
					if (!available(1, sr))
						goto failure;
					sr += utf8::size_of_first_rune(input_.cbegin() + sr, input_.cend());
				} break;
				case opcode::match_class: {
					if (!available(1, sr))
						goto failure;
					auto first = input_.cbegin() + sr;
					auto [rune, next] = utf8::decode_rune(first, input_.cend());
					auto record = unicode::query(rune);
					bool match = false;
					switch (alt) {
						case altcode::match_ptype: match = record.any_of(detail::string_unpack<unicode::ptype>(str)); break;
						case altcode::match_gctype: match = record.any_of(detail::string_unpack<unicode::gctype>(str)); break;
						case altcode::match_sctype: match = record.script() == static_cast<unicode::sctype>(imm); break;
						default: match = record.any_of(static_cast<unicode::ctype>(imm)); break;
					}
					if (!match)
						goto failure;
					sr += std::distance(first, next);
				} break;
				case opcode::match_range: {
					std::string_view first = str.substr(0, imm), last = str.substr(imm);
					if (!available((std::min)(first.size(), last.size()), sr))
						goto failure;
					auto sz = utf8::size_of_first_rune(input_.cbegin() + sr, input_.cend());
					if (input_.compare(sr, sz, first) < 0 || input_.compare(sr, sz, last) > 0)
						goto failure;
					sr += sz;
				} break;
				case opcode::choice: {
					stack_frames_.push_back(stack_frame_type::backtrack);
					backtrack_stack_.emplace_back(sr - imm, rc, pc + off);
				} break;
				case opcode::commit: {
					if (stack_frames_.empty() || stack_frames_.back() != stack_frame_type::backtrack)
						goto failure;
					switch (alt) {
						case altcode::commit_partial: detail::make_tuple_view<0, 1>(backtrack_stack_.back()) = {sr, rc}; break;
						case altcode::commit_back: sr = std::get<0>(backtrack_stack_.back()); [[fallthrough]];
						default: pop_stack_frame(backtrack_stack_); break;
					}
				} [[fallthrough]];
				case opcode::jump: {
					pc += off;
				} break;
				case opcode::call: {
					if (imm != 0) {
						auto memo = detail::escaping_find_if(lrmemo_stack_.crbegin(), lrmemo_stack_.crend(),
								[sr = sr, pca = pc + off](const auto& m) { return m.srr == sr && m.pca == pca ? 1 : (m.srr < sr ? 0 : -1); });
						if (memo != lrmemo_stack_.crend()) {
							if (memo->sra == lrfailcode || imm < memo->prec)
								goto failure;
							sr = memo->sra, rc = semantics_.restore_responses_after(rc, memo->responses);
							continue;
						}
						stack_frames_.push_back(stack_frame_type::lrcall);
						lrmemo_stack_.push_back({sr, lrfailcode, imm, pc, pc + off, rc, std::vector<semantic_response>{}});
					} else {
						stack_frames_.push_back(stack_frame_type::call);
						call_stack_.push_back(pc);
					}
					pc += off;
				} break;
				case opcode::ret: {
					if (stack_frames_.empty())
						goto failure;
					switch (stack_frames_.back()) {
						case stack_frame_type::call: {
							pc = call_stack_.back();
							pop_stack_frame(call_stack_);
						} break;
						case stack_frame_type::lrcall: {
							auto& memo = lrmemo_stack_.back();
							if (memo.sra == lrfailcode || sr > memo.sra) {
								memo.sra = sr, memo.responses = semantics_.drop_responses_after(memo.rcr);
								sr = memo.srr, pc = memo.pca, rc = memo.rcr;
								continue;
							}
							sr = memo.sra, pc = memo.pcr, rc = semantics_.restore_responses_after(memo.rcr, memo.responses);
							pop_stack_frame(lrmemo_stack_, sr, mr, rc, pc);
						} break;
						default: goto failure;
					}
				} break;
				case opcode::fail: {
					fc = imm;
				failure:
					if (sr > mr)
						mr = sr;
					for (++fc; fc > 0; --fc) {
						if (done = cut_frame_ >= stack_frames_.size(); done) {
							registers_ = {sr, mr, rc, pc, 0};
							break;
						}
						switch (stack_frames_.back()) {
							case stack_frame_type::backtrack: {
								std::tie(sr, rc, pc) = backtrack_stack_.back();
								pop_stack_frame(backtrack_stack_);
							} break;
							case stack_frame_type::call: {
								pop_stack_frame(call_stack_), ++fc;
							} break;
							case stack_frame_type::capture: {
								pop_stack_frame(capture_stack_, sr, mr, rc, pc), ++fc;
							} break;
							case stack_frame_type::lrcall: {
								auto& memo = lrmemo_stack_.back();
								if (memo.sra != lrfailcode) {
									sr = memo.sra, pc = memo.pcr, rc = semantics_.restore_responses_after(memo.rcr, memo.responses);
								} else ++fc;
								pop_stack_frame(lrmemo_stack_, sr, mr, rc, pc);
							} break;
							default: break;
						}
					}
					semantics_.pop_responses_after(rc);
				} break;
				case opcode::accept: {
					if (cut_deferred_ = !capture_stack_.empty() || !lrmemo_stack_.empty(); !cut_deferred_) {
						accept(sr, mr, rc, pc);
						if (result = done = alt == altcode::accept_final; done)
							break;
					}
				} break;
				case opcode::predicate: {
					registers_ = {sr, sr > mr ? sr : mr, rc, pc, 0};
					bool accepted = prog.predicates[imm](*this);
					std::tie(sr, mr, rc, pc, fc) = registers_.as_tuple();
					semantics_.pop_responses_after(rc);
					if (!accepted)
						goto failure;
				} break;
				case opcode::action: {
					rc = semantics_.push_response(call_stack_.size() + lrmemo_stack_.size(), imm);
				} break;
				case opcode::begin_capture: {
					stack_frames_.push_back(stack_frame_type::capture);
					capture_stack_.push_back(static_cast<subject>(sr));
				} break;
				case opcode::end_capture: {
					if (stack_frames_.empty() || stack_frames_.back() != stack_frame_type::capture)
						goto failure;
					auto sr0 = static_cast<std::size_t>(capture_stack_.back()), sr1 = sr;
					pop_stack_frame(capture_stack_, sr, mr, rc, pc);
					if (sr0 > sr1)
						goto failure;
					rc = semantics_.push_capture_response(call_stack_.size() + lrmemo_stack_.size(), imm, sr0, sr1 - sr0);
				} break;
				default: registers_ = {sr, sr > mr ? sr : mr, rc, pc, 0}; throw bad_opcode{};
			}
		}
		return result;
	}
};

template <class InputIt, class = utf8::enable_if_char_input_iterator_t<InputIt>>
inline bool parse(InputIt first, InputIt last, const grammar& grmr, semantics& sema) {
	return parser{grmr, sema}.enqueue(first, last).parse();
}

template <class InputIt, class = utf8::enable_if_char_input_iterator_t<InputIt>>
inline bool parse(InputIt first, InputIt last, const grammar& grmr) {
	semantics sema;
	return parse(first, last, grmr, sema);
}

inline bool parse(std::istream& input, const grammar& grmr, semantics& sema) {
	return parser{grmr, sema}.push_source([&input](std::string& line) {
		if (std::getline(input, line)) {
			line.push_back('\n');
			return true;
		}
		return false;
	}).parse();
}

inline bool parse(std::istream& input, const grammar& grmr) {
	semantics sema;
	return parse(input, grmr, sema);
}

inline bool parse(std::string_view sv, const grammar& grmr, semantics& sema) { return parse(sv.cbegin(), sv.cend(), grmr, sema); }
inline bool parse(std::string_view sv, const grammar& grmr) { return parse(sv.cbegin(), sv.cend(), grmr); }
inline bool parse(const grammar& grmr, semantics& sema) { return parse(std::cin, grmr, sema); }
inline bool parse(const grammar& grmr) { return parse(std::cin, grmr); }

inline grammar string_expression::make_grammar() {
	using namespace language;
	rule Empty = eps                                                            <[](generator& g) { g.encoder.encode(opcode::match); };
	rule Dot = chr('.')                                                         <[](generator& g) { g.encoder.encode(opcode::match_any).dclr(directives::matcheps); };
	rule Element = any > chr('-') > !chr(']') > any                             <[](generator& g, syntax x) { g.bracket_range(x.capture); }
		| chr('[') > chr(':') > +(!chr(':') > any) > chr(':') > chr(']')        <[](generator& g, syntax x) { g.bracket_class(x.capture.substr(2, x.capture.size() - 4)); }
		| any                                                                   <[](generator& g, syntax x) { g.bracket_range(x.capture, x.capture); };
	rule Bracket = chr('[') > ~(chr('^')                                        <[](generator& g) { g.circumflex = true; })
		> Element > *(!chr(']') > Element) > chr(']')                           <[](generator& g) { g.bracket_commit(); };
	rule Sequence = +(!(chr('.') | chr('[')) > any)                             <[](generator& g, syntax x) { g.encoder.match(x.capture); };
	return start((+(Dot | Bracket | Sequence) | Empty) > eoi);
}

inline void string_expression::compile(std::string_view sv) {
	static const grammar grmr = make_grammar();
	generator genr(*this);
	if (!parse(sv, grmr, genr))
		throw bad_string_expression{};
}

} // namespace lug

#endif
