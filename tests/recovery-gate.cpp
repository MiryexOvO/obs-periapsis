// Check request storms, slow encoders and fresh requests after recovery.
// 验证请求风暴、编码器无响应和恢复后的新请求。
#include "../native/recovery-gate.hpp"
#include <cassert>
int main()
{
    RecoveryGate gate;
    for (int i = 0; i < 100000; ++i) gate.request();
    assert(gate.due(0));
    gate.submitted(0, 2);
    assert(!gate.observed(2));
    gate.request();
    assert(!gate.due(500));
    assert(gate.observed(3));
    assert(!gate.pending);
    gate.request(3);
    assert(!gate.due(332));
    assert(gate.due(333));
    gate.submitted(333, 3);
    assert(!gate.expired(1332));
    assert(gate.expired(1333));
    assert(!gate.due(2000)); // No endless retry without fresh demand. / 无新需求不无限重试。
    gate.request();
    assert(gate.due(2000));
    gate.submitted(2000,3);gate.request(4);
    assert(gate.observed(4));assert(gate.pending);
}
