#ifndef QSIM_VISUALIZATION_SDL_H
#define QSIM_VISUALIZATION_SDL_H

#include <SDL3/SDL.h>
#ifdef __COSMOPOLITAN__
#include <libc/dce.h>
#endif

namespace visualization {

	// Cosmopolitan uses System V on Windows too. Native SDL uses the Windows ABI.
	template <typename Pointer> struct SdlFunction;

	template <typename Result, typename... Args> struct SdlFunction<Result (*)(Args...)> {
		Result (*system_v)(Args...) = nullptr;
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
		Result(__attribute__((ms_abi)) * windows)(Args...) = nullptr;
#endif
		Result operator()(Args... args) const {
#if defined(__COSMOPOLITAN__) && defined(__x86_64__)
			if (IsWindows()) return windows(args...);
#endif
			return system_v(args...);
		}
	};

#define QSIM_SDL_FUNCTIONS(F)              \
	F(SDL_ConvertEventToRenderCoordinates) \
	F(SDL_CreateRenderer)                  \
	F(SDL_CreateWindow)                    \
	F(SDL_DestroyRenderer)                 \
	F(SDL_DestroyWindow)                   \
	F(SDL_GetCurrentRenderOutputSize)      \
	F(SDL_GetDisplayContentScale)          \
	F(SDL_GetError)                        \
	F(SDL_GetKeyboardState)                \
	F(SDL_GetPrimaryDisplay)               \
	F(SDL_GetRenderScale)                  \
	F(SDL_GetTicks)                        \
	F(SDL_GetWindowDisplayScale)           \
	F(SDL_Init)                            \
	F(SDL_PollEvent)                       \
	F(SDL_Quit)                            \
	F(SDL_RenderClear)                     \
	F(SDL_RenderFillRect)                  \
	F(SDL_RenderLine)                      \
	F(SDL_RenderPoint)                     \
	F(SDL_RenderPresent)                   \
	F(SDL_RenderRect)                      \
	F(SDL_SetHint)                         \
	F(SDL_SetRenderDrawBlendMode)          \
	F(SDL_SetRenderDrawColor)              \
	F(SDL_SetRenderScale)

	struct SdlApi {
#define QSIM_DECLARE_SDL(name) SdlFunction<decltype(&::name)> name;
		QSIM_SDL_FUNCTIONS(QSIM_DECLARE_SDL)
#undef QSIM_DECLARE_SDL
	};

	extern SdlApi sdl;
	void          load_sdl();

} // namespace visualization

#endif
