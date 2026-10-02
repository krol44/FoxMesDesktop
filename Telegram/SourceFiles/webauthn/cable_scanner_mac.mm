#include "webauthn/cable_scanner.h"
namespace Platform::WebAuthn::Cable {
std::unique_ptr<BleScanner> MakeBleScanner() { return nullptr; }
} // namespace Platform::WebAuthn::Cable
