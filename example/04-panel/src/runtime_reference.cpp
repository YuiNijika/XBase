#include <XBase/Panel.h>

// 只供运行时所有者参考 普通宿主不会调用本文件里的函数
// 共享运行时已由核心调度面板 再驱动一次会重复处理全局热键
namespace PanelSample::RuntimeOwnerReference {

void Initialize() {
    if (!XBase::Panel::IsInitialized()) XBase::Panel::Init();
}

void WorldReady() {
    XBase::Panel::NotifyGameInit();
}

void ProcessFrame() {
    if (XBase::Panel::IsInitialized()) XBase::Panel::Process();
}

void Shutdown() {
    if (XBase::Panel::IsInitialized()) XBase::Panel::Shutdown();
}

}
