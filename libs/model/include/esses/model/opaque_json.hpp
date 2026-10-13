// SPDX-FileCopyrightText: 2026 the Esses contributors
// SPDX-License-Identifier: MIT
//
// OpaqueJson: a JSON value the model carries without interpreting it: a sidecar layer's localMaths module
// (maths-1, validated by esses_project), the reserved runs/stages members, unknown top-level members of a
// document (02 section 6.1 Document<T>::unknown) and the source's probe_json.
//
// It is esses_base's JsonDoc (01 section 2.1; M1): the value is held as its compact canonical text, so two
// equal values compare equal and no JSON library type crosses the esses_model boundary. The name stays for
// the M0/M1 code that spells it; new code writes JsonDoc.
#pragma once

#include <esses/base/json_doc.hpp>

namespace esses::model {

using OpaqueJson = ::esses::JsonDoc;

} // namespace esses::model
