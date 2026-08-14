#ifndef SIM_PACING_H
#define SIM_PACING_H

namespace sim {

	// The run can fall behind its target speed; it never goes faster than its slowest application.
	struct Pacing {
		double configured = 1.0;
		double effective  = 1.0;
		bool   behind     = false;
	};

	// Called by the engine at the end of a frame's work.
	void publish_pacing(const Pacing& pacing);

	// Safe to call from the render thread.
	[[nodiscard]] Pacing pacing();

} // namespace sim

#endif // SIM_PACING_H
