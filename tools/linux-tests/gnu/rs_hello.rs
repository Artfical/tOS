fn main() {
    let v: Vec<u32> = (1..=10).collect();
    let s: u32 = v.iter().sum();
    println!("hello from Rust on tOS: sum={}", s);
    let h = std::thread::spawn(|| 6 * 7);
    println!("thread says {}", h.join().unwrap());
}
