#include "PCH.h"

#include "Hooks/FrameHooks.h"

#include "Hooks/Detours.h"
#include "Telemetry/Zone.h"

// Address Library IDs are (SE, AE). Signatures and IDs cross-checked against
// CommonLibSSE and Community Shaders' FrameAnnotations/Hooks.

namespace EngineTelemetry::FrameHooks
{
	namespace
	{
		using namespace std::literals;

		constexpr auto kCategoryFrame = "Frame";
		constexpr auto kCategoryRender = "Render";

		// Skyrim's main loop body. Rendering for the frame (Main::RenderPlayerView,
		// Renderer::Begin/End) is driven from inside this call on the main thread.
		struct Main_Update
		{
			static constexpr auto     name = "Main::Update"sv;
			static constexpr REL::ID  id = RELOCATION_ID(35565, 36564);

			static void thunk(RE::Main* a_this, float a_unk)
			{
				static std::atomic_bool threadNamed{ false };
				if (!threadNamed.exchange(true, std::memory_order_relaxed)) {
					tracy::SetThreadName("Skyrim Main");
				}

				{
					basic_telemetry::Frame frame{ "Skyrim Main" };
					SET_ZONE("Main::Update", kCategoryFrame);
					original(a_this, a_unk);
				}
				BT_FRAME_MARK();
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Main_RenderPlayerView
		{
			static constexpr auto    name = "Main::RenderPlayerView"sv;
			static constexpr REL::ID id = RELOCATION_ID(35560, 36559);

			static void thunk(void* a_unk1, bool a_unk2, bool a_unk3)
			{
				SET_ZONE("Main::RenderPlayerView", kCategoryRender);
				original(a_unk1, a_unk2, a_unk3);
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Main_RenderDepth
		{
			static constexpr auto    name = "Main::RenderDepth"sv;
			static constexpr REL::ID id = RELOCATION_ID(100421, 107139);

			static void thunk(bool a_unk1, bool a_unk2)
			{
				SET_ZONE("Main::RenderDepth", kCategoryRender);
				original(a_unk1, a_unk2);
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Main_RenderShadowmasks
		{
			static constexpr auto    name = "Main::RenderShadowmasks"sv;
			static constexpr REL::ID id = RELOCATION_ID(100422, 107140);

			static void thunk(bool a_unk)
			{
				SET_ZONE("Main::RenderShadowmasks", kCategoryRender);
				original(a_unk);
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Main_RenderWorld
		{
			static constexpr auto    name = "Main::RenderWorld"sv;
			static constexpr REL::ID id = RELOCATION_ID(100424, 107142);

			static void thunk(bool a_unk)
			{
				SET_ZONE("Main::RenderWorld", kCategoryRender);
				original(a_unk);
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Main_RenderFirstPersonView
		{
			static constexpr auto    name = "Main::RenderFirstPersonView"sv;
			static constexpr REL::ID id = RELOCATION_ID(100411, 107129);

			static void thunk(bool a_unk1, bool a_unk2)
			{
				SET_ZONE("Main::RenderFirstPersonView", kCategoryRender);
				original(a_unk1, a_unk2);
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Main_RenderWaterEffects
		{
			static constexpr auto    name = "Main::RenderWaterEffects"sv;
			static constexpr REL::ID id = RELOCATION_ID(35561, 36560);

			static void thunk()
			{
				SET_ZONE("Main::RenderWaterEffects", kCategoryRender);
				original();
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		// BSGraphics::Renderer::Begin/End are private in CommonLibSSE, so `this`
		// is taken as void*. End() presents the swap chain.
		struct Renderer_Begin
		{
			static constexpr auto    name = "BSGraphics::Renderer::Begin"sv;
			static constexpr REL::ID id = RELOCATION_ID(75460, 77245);

			static void thunk(void* a_this, std::uint32_t a_windowID)
			{
				SET_ZONE("BSGraphics::Renderer::Begin", kCategoryRender);
				original(a_this, a_windowID);
			}

			static inline decltype(&thunk) original{ nullptr };
		};

		struct Renderer_End
		{
			static constexpr auto    name = "BSGraphics::Renderer::End"sv;
			static constexpr REL::ID id = RELOCATION_ID(75461, 77246);

			static void thunk(void* a_this)
			{
				SET_ZONE("BSGraphics::Renderer::End (Present)", kCategoryRender);
				original(a_this);
			}

			static inline decltype(&thunk) original{ nullptr };
		};
	}

	bool Install()
	{
		DetourTransaction transaction;
		AttachFunctionDetour<Main_Update>(transaction);
		AttachFunctionDetour<Main_RenderPlayerView>(transaction);
		AttachFunctionDetour<Main_RenderDepth>(transaction);
		AttachFunctionDetour<Main_RenderShadowmasks>(transaction);
		AttachFunctionDetour<Main_RenderWorld>(transaction);
		AttachFunctionDetour<Main_RenderFirstPersonView>(transaction);
		AttachFunctionDetour<Main_RenderWaterEffects>(transaction);
		AttachFunctionDetour<Renderer_Begin>(transaction);
		AttachFunctionDetour<Renderer_End>(transaction);
		return transaction.Commit();
	}
}
