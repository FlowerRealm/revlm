#pragma once

#include <odb/database.hxx>

namespace revlm
{

// Applies ODB baseline schema (empty DB) then versioned SQL under backend/migrations/.
void ensure_schema(odb::database &db);

} // namespace revlm
