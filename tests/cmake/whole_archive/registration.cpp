extern int registrations;

namespace {

// 独立翻译单元仅含静态注册，验证整库链接仍保留无人显式引用的初始化对象。
struct Registration {
    Registration() { ++registrations; }
};

Registration registration;

}
