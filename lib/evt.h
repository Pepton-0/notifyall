#pragma once
// Subject/observer. Expose IEvent& (connect only) and keep event (publish) to the owner.
// Not thread safe: callers serialize publish and must not hold their lock while handlers run.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace evt {

	using token = std::uint64_t;

	namespace detail {

		// Owned by the event via shared_ptr; subscriptions hold a weak_ptr so they may outlive it.
		template <class... Args>
		class channel {
		public:
			struct entry { token id; std::function<void(Args...)> fn; };

			token add(std::function<void(Args...)> fn)
			{
				const token t = ++last;
				auto& target = publishing() ? added_while_publishing : handlers;
				target.push_back({ t, std::move(fn) });
				return t;
			}

			// Only marks the entry so that a running publish loop stays valid.
			void remove(token t)
			{
				mark_removed(handlers, t);
				mark_removed(added_while_publishing, t);
				if (!publishing()) compact();
			}

			void compact()
			{
				for (auto& e : added_while_publishing)
					if (e.fn) handlers.push_back(std::move(e));
				added_while_publishing.clear();
				std::erase_if(handlers, [](const entry& e) { return !e.fn; });
			}

			std::size_t live() const
			{
				return count_live(handlers) + count_live(added_while_publishing);
			}

			bool publishing() const { return depth > 0; }

			std::vector<entry> handlers;
			std::vector<entry> added_while_publishing;
			token last = 0;
			int depth = 0;

		private:
			static void mark_removed(std::vector<entry>& v, token t)
			{
				for (auto& e : v)
					if (e.id == t) { e.id = 0; e.fn = nullptr; return; }
			}

			static std::size_t count_live(const std::vector<entry>& v)
			{
				std::size_t n = 0;
				for (const auto& e : v) if (e.fn) ++n;
				return n;
			}
		};

	} // namespace detail

	// Unsubscribes on destruction. Does nothing if the event is already gone. Move only.
	template <class... Args>
	class subscription {
	public:
		subscription() = default;
		subscription(std::weak_ptr<detail::channel<Args...>> ch, token t)
			: ch(std::move(ch)), t(t) {}

		subscription(const subscription&) = delete;
		subscription& operator=(const subscription&) = delete;

		subscription(subscription&& o) noexcept : ch(std::move(o.ch)), t(o.t) { o.ch.reset(); }
		subscription& operator=(subscription&& o) noexcept
		{
			if (this != &o) { reset(); ch = std::move(o.ch); t = o.t; o.ch.reset(); }
			return *this;
		}

		~subscription() { reset(); }

		void reset()
		{
			if (const auto c = ch.lock()) c->remove(t);
			ch.reset();
		}

		// False once the event is destroyed.
		bool active() const { return !ch.expired(); }

	private:
		std::weak_ptr<detail::channel<Args...>> ch;
		token t = 0;
	};

	// Subscribe-only view. The protected destructor forbids deleting through it.
	template <class... Args>
	class IEvent {
	public:
		IEvent(const IEvent&) = delete;
		IEvent& operator=(const IEvent&) = delete;

		subscription<Args...> connect(std::function<void(Args...)> handler)
		{
			return subscription<Args...>(ch, ch->add(std::move(handler)));
		}

		std::size_t count() const { return ch->live(); }

	protected:
		IEvent() : ch(std::make_shared<detail::channel<Args...>>()) {}
		~IEvent() = default;

		std::shared_ptr<detail::channel<Args...>> ch;
	};

	template <class... Args>
	class event final : public IEvent<Args...> {
	public:
		// Handlers connected during publish run from the next publish.
		// A throwing handler stops the remaining ones and propagates.
		void publish(Args... args)
		{
			auto& c = *this->ch;

			struct depth_guard {
				detail::channel<Args...>& c;
				explicit depth_guard(detail::channel<Args...>& c) : c(c) { ++c.depth; }
				~depth_guard() { if (--c.depth == 0) c.compact(); }
			};

			const std::size_t existing = c.handlers.size();
			const depth_guard guard{ c };

			for (std::size_t i = 0; i < existing; ++i)
				if (c.handlers[i].fn) c.handlers[i].fn(args...);
		}
	};

} // namespace evt
