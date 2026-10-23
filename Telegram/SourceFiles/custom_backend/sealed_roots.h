#pragma once

#include <array>
#include <string_view>

namespace CustomBackend::Sealed {

struct RootKey {
	std::string_view name;
	std::string_view pub;
};

inline constexpr auto kReleaseRoots = std::array{
	RootKey{ "main", "BH2lkBMPeHXjXbYfb3LXCmqlTSpwauxFBFeGa5c5wpcvfRy5EMazkLO8rJJn5x0l9Mwof2be0IgFQEt87qu41Pc" },
	RootKey{ "backup", "BJCTSzIuUbMQkEmXT8GKbPDVLY-GKecfl9CorLWUK6vxKqssYNGx1LAnC4jZgpKUHEWTUliIO-bxo6w26Je7pzY" },
};

#if FOXMES_ALLOW_ENDPOINT_OVERRIDE
inline constexpr auto kStageRoot = RootKey{
	"stage",
	"BGaLTyW3v-B9KgdSD87VxACK1pX5omaD_Mq8pjttI6lVnj2QH0EF_gN5NqZ_okrP04lBu2TznNOMwjGRrKdkSKw",
};
#endif // FOXMES_ALLOW_ENDPOINT_OVERRIDE

inline constexpr auto kMinimalSerial = 1;

} // namespace CustomBackend::Sealed
