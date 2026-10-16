// 普通符号引用会先抽取本对象，用于重现随后整库装载造成的重复定义。
int registrations = 0;

int RegisteredCount() {
    return registrations;
}
