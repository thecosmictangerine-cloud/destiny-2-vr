#include "activity_defaults_snapshot.h"

#include <Windows.h>

#include "../../runtime/storage/internal.h"

namespace sunrise::state::activity::defaults {

/** Copies the immutable activity defaults published with the root State. */
void snapshot(ActivityDefaults& output) noexcept {
    AcquireSRWLockShared(&runtime::storage::g_stateLock);
    output = runtime::storage::g_state.activity.defaults;
    ReleaseSRWLockShared(&runtime::storage::g_stateLock);
}

} // namespace sunrise::state::activity::defaults
