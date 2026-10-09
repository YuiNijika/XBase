#pragma once

// 共享运行时把 Panel 的 C 接口转发到本地注册表用的跳板。
// 这些函数不走 Panel::Mount 那条转发链，否则共享库会自己调回自己

namespace XBase::Panel::Abi {

int MountJson(const char* specJson);
void UnmountName(const char* modId);
int BindValueRaw(const char* controlId, double (*read)(void*), void (*write)(double, void*), void* userData);
int BindTextRaw(const char* controlId, int (*read)(char*, std::uint32_t, void*), void (*write)(const char*, void*), void* userData);
int BindActionRaw(const char* controlId, void (*run)(void*), void* userData);
void NotifyChangedName(const char* controlId, double value);
void NotifyTextChangedName(const char* controlId, const char* value);
int Available();
void ShowName(const char* modId);
void HideName();
int IsVisibleRaw();
void SetHotkeyRaw(int key, unsigned int modifiers);

} // namespace XBase::Panel::Abi
