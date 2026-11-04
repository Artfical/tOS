# tkinter on tOS: a window on the X server, widgets, events, and what the pointer and keys do in it
import sys, tkinter as tk
from tkinter import ttk
fails = 0
def check(cond, what):
    global fails
    if cond: print("tktest: ok", what, flush=True)
    else: print("tktest: FAIL", what, flush=True); fails += 1
root = tk.Tk()
root.title("tkinter on tOS")
root.geometry("500x380+20+20")
check(root.winfo_screenwidth() == 800 and root.winfo_screenheight() == 560, "the screen is the tOS window: %dx%d" % (root.winfo_screenwidth(), root.winfo_screenheight()))
check(tk.TkVersion >= 8.6, "Tk %s" % tk.TkVersion)
clicked = []
tk.Label(root, text="Hello from tkinter on tOS", font=("fixed", 14)).pack(pady=8)
b = tk.Button(root, text="Click me", command=lambda: clicked.append(1)); b.pack()
e = tk.Entry(root); e.pack(pady=6)
c = tk.Canvas(root, width=300, height=120, bg="white"); c.pack()
c.create_rectangle(10, 10, 140, 100, fill="#2040c0"); c.create_oval(160, 10, 290, 100, fill="#c02020"); c.create_text(150, 60, text="canvas", fill="white")
lb = tk.Listbox(root, height=3); [lb.insert("end", t) for t in ("one", "two", "three")]; lb.pack()
cb = ttk.Combobox(root, values=("a", "b", "c")); cb.pack(pady=4)
root.update()
check(root.winfo_ismapped() and b.winfo_width() > 20, "the window is mapped and laid out (%dx%d button)" % (b.winfo_width(), b.winfo_height()))
b.invoke(); check(clicked == [1], "a button's command runs")
e.insert(0, "typed"); check(e.get() == "typed", "an Entry takes text")
root.after(700, lambda: print("tktest: waiting for a click on the button and keys in the entry (6 s)", flush=True))
seen = {"click": 0, "key": 0}
b.bind("<Button-1>", lambda ev: seen.__setitem__("click", seen["click"] + 1))
e.bind("<Key>", lambda ev: seen.__setitem__("key", seen["key"] + 1))
root.after(7000, root.quit)
root.mainloop()
print("tktest: pointer clicks on the button: %d, keys in the entry: %d (0 when nobody used the window)" % (seen["click"], seen["key"]), flush=True)
root.destroy()
print("tktest: all passed" if not fails else "tktest: %d FAILED" % fails, flush=True)
sys.exit(1 if fails else 0)
