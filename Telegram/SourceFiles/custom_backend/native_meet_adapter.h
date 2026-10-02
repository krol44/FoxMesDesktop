/*
This file is part of FoxMes Desktop.
*/
#pragma once

#include "base/basic_types.h"

class History;
class PeerData;

namespace CustomBackend::Meet {


[[nodiscard]] bool Available(PeerData *peer);

void Start(not_null<History*> history);

}
