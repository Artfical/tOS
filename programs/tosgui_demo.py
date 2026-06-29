import tosgui

count = 0

def draw():
    tosgui.clear()
    tosgui.text(2, 2, "tosgui demo - click counter", tosgui.LIGHT_CYAN)
    tosgui.text(2, 4, "Clicks: " + str(count), tosgui.YELLOW)
    tosgui.button(2, 6, "Click me", tosgui.WHITE, tosgui.BLUE)
    tosgui.button(2, 8, "Quit", tosgui.WHITE, tosgui.RED)
    tosgui.text(2, 11, "(or press q to quit)", tosgui.DARK_GREY)

if not tosgui.open("tosgui demo"):
    print("a tosgui window is already open")
else:
    draw()
    running = True
    while running:
        click = tosgui.poll_click()
        if click:
            x, y = click
            if y == 6 and 2 <= x <= 12:
                count += 1
                draw()
            elif y == 8 and 2 <= x <= 8:
                running = False

        key = tosgui.poll_key()
        if key is not None and chr(key) == 'q':
            running = False

        tosgui.update()

    tosgui.close()
    print("tosgui demo closed, total clicks:", count)
