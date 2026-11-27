/* Lists the windows of the X server: id, position, size, whether mapped. Looks at what a program (GTK) really made.
 *   gcc xtree.c -o xtree -lX11     then   tos-x sh -c "program & sleep 20; xtree" */
#include <X11/Xlib.h>
#include <stdio.h>

static void walk(Display *d, Window w, int depth)
{
    XWindowAttributes a;
    if (!XGetWindowAttributes(d, w, &a)) return;
    printf("%*s0x%lx %dx%d+%d+%d %s class=%d\n", depth * 2, "", w, a.width, a.height, a.x, a.y, a.map_state == IsViewable ? "viewable" : a.map_state == IsUnviewable ? "unviewable" : "unmapped", a.class);
    Window root, parent, *kids; unsigned n;
    if (XQueryTree(d, w, &root, &parent, &kids, &n)) { for (unsigned i = 0; i < n; i++) walk(d, kids[i], depth + 1); if (kids) XFree(kids); }
}

int main(void)
{
    Display *d = XOpenDisplay(0);
    if (!d) { printf("xtree: no display\n"); return 1; }
    int ne = 0; char **ex = XListExtensions(d, &ne);
    printf("extensions:");
    for (int i = 0; i < ne; i++) printf(" %s", ex[i]);
    printf("\n");
    walk(d, DefaultRootWindow(d), 0);
    XCloseDisplay(d);
    return 0;
}
