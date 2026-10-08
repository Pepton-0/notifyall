#include <vector>

#include "composition.h"
#include "framework.h"
#include "notifyinjector.h"
#include "tray_icon.h"

namespace {

	// Records the on/off calls in order.
	class fake_injector final : public i_notifyinjector {
	public:
		void on() override { calls.push_back(true); active_ = true; }
		void off() override { calls.push_back(false); active_ = false; }
		bool active() const override { return active_; }

		std::vector<bool> calls;

	private:
		bool active_ = false;
	};

}

test::suite notifyinjector_suite()
{
	return { "notifyinjector", {
		{ "on_off", [](test::context& t) {
			notifyinjector n;
			CHECK(t, !n.active());
			n.on();
			CHECK(t, n.active());
			n.off();
			CHECK(t, !n.active());
		} },
	} };
}

test::suite tray_icon_suite()
{
	// run() needs a real window and tray, so only the state changes are tested.
	return { "tray_icon", {
		{ "visible_by_default", [](test::context& t) {
			fake_injector f;
			tray_icon tray(f);
			CHECK(t, tray.visible());
			CHECK(t, f.calls.empty());
		} },
		{ "toggle_switches_injector", [](test::context& t) {
			fake_injector f;
			tray_icon tray(f);

			std::vector<bool> published;
			auto sub = tray.on_visibility_changed().connect([&](bool v) { published.push_back(v); });

			tray.toggle();
			CHECK(t, !tray.visible());
			CHECK(t, !f.active());

			tray.toggle();
			CHECK(t, tray.visible());
			CHECK(t, f.active());

			CHECK(t, (f.calls == std::vector<bool>{ false, true }));
			CHECK(t, (published == std::vector<bool>{ false, true }));
		} },
	} };
}

test::suite composition_suite()
{
	return { "composition", {
		{ "resolves_singletons", [](test::context& t) {
			auto injector = composition::make_injector();
			auto& tray1 = injector.create<i_tray_icon&>();
			auto& tray2 = injector.create<i_tray_icon&>();
			auto& inj1 = injector.create<i_notifyinjector&>();

			CHECK(t, &tray1 == &tray2);
			CHECK(t, dynamic_cast<tray_icon*>(&tray1) != nullptr);
			CHECK(t, dynamic_cast<notifyinjector*>(&inj1) != nullptr);

			// The tray must have received this very notifyinjector.
			tray1.toggle();
			CHECK(t, !inj1.active());
			tray1.toggle();
			CHECK(t, inj1.active());
			inj1.off();
		} },
	} };
}
