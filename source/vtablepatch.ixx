module;

#include <common.hxx>

export module vtablepatch;

// One entry of a COM vtable written through injector (the page is read only).
export void WriteVTableEntry(void** entry, void* value)
{
    injector::WriteMemory(entry, value, true);
}

// Puts hook in vtable[index] and keeps the entry it replaces in real, for the hook to call on. Nothing when the
// entry is the hook already, so installing again, as on a device reset, never makes the hook call itself.
export template <class F>
void PatchVTable(void** vtable, int index, F& real, F hook)
{
    if (vtable[index] == reinterpret_cast<void*>(hook))
        return;
    real = reinterpret_cast<F>(vtable[index]);
    WriteVTableEntry(&vtable[index], reinterpret_cast<void*>(hook));
}
