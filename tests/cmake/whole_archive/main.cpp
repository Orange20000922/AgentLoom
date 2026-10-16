#include <iostream>

int RegisteredCount();

int main() {
    // CTest 检查输出；本程序是无 SDK 依赖的链接夹具，不建立自定义断言框架。
    std::cout << "registered=" << RegisteredCount() << '\n';
}
