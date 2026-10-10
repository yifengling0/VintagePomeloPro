#!/usr/bin/env python3
"""Execute the production front-export function with an instrumented DRI context."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / 'thirdparty/mesa/src/gallium/frontends/dri/dri_drawable.c').read_text()
start = source.index('bool\ndriWineHuaPresentFrontBuffer(')
body = source.index('{', start)
depth, end = 1, body + 1
while depth:
    depth += (source[end] == '{') - (source[end] == '}')
    end += 1
production = source[start:end]
code = r"""
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#define DRI_SCREEN_SWRAST 1
#define DRI_SCREEN_KMS_SWRAST 2
#define ST_ATTACHMENT_FRONT_LEFT 0
#define ST_FLUSH_END_OF_FRAME 4
struct pipe_screen { const char *(*get_name)(struct pipe_screen *); };
struct dri_screen { struct { struct pipe_screen *screen; } base; int type; };
struct st_context { void *ctx; };
struct dri_context;
struct dri_drawable {
    struct dri_screen *screen; void *textures[1];
    bool (*flush_frontbuffer)(struct dri_context *, struct dri_drawable *, unsigned);
};
struct dri_context {struct dri_drawable *draw; struct dri_screen *screen; struct st_context *st;};
static struct dri_context *current;
static int order, finishes, flushes, exports;
static bool export_ok = true;
static const char *driver = "virgl";
static struct dri_context *dri_get_current(void) {return current;}
static const char *get_name(struct pipe_screen *s) {(void)s;return driver;}
static void _mesa_glthread_finish(void *ctx) {assert(ctx);assert(order==0);order=1;finishes++;}
static void st_context_flush(struct st_context *s, unsigned flags,void*a,void*b,void*c)
{assert(s);assert(order==1);assert(flags==ST_FLUSH_END_OF_FRAME);assert(!a&&!b&&!c);order=2;flushes++;}
static bool export_front(struct dri_context *ctx,struct dri_drawable *d,unsigned attachment)
{assert(ctx==current&&ctx->draw==d);assert(order==2&&attachment==ST_ATTACHMENT_FRONT_LEFT);order=3;exports++;return export_ok;}
""" + production + r"""
static void reset(void) {order=finishes=flushes=exports=0;}
int main(void) {
    struct pipe_screen ps={get_name}; struct dri_screen screen={{&ps},DRI_SCREEN_SWRAST};
    struct dri_screen other={{&ps},DRI_SCREEN_SWRAST};struct st_context st={(void*)1};
    struct dri_drawable draw={&screen,{(void*)1},export_front}, wrong=draw;
    struct dri_context ctx={&draw,&screen,&st};
    assert(!driWineHuaPresentFrontBuffer(&draw));assert(!order);
    current=&ctx;assert(!driWineHuaPresentFrontBuffer(NULL));assert(!order);
    assert(!driWineHuaPresentFrontBuffer(&wrong));assert(!order);
    ctx.screen=&other;assert(!driWineHuaPresentFrontBuffer(&draw));assert(!order);ctx.screen=&screen;
    driver="softpipe";assert(driWineHuaPresentFrontBuffer(&draw));assert(!order);driver="virgl";
    screen.type=3;assert(driWineHuaPresentFrontBuffer(&draw));assert(!order);screen.type=DRI_SCREEN_SWRAST;
    assert(driWineHuaPresentFrontBuffer(&draw));assert(order==3&&finishes==1&&flushes==1&&exports==1);
    reset();assert(driWineHuaPresentFrontBuffer(&draw));assert(exports==1); /* earlier glFlush need not leave dirty state */
    reset();export_ok=false;assert(!driWineHuaPresentFrontBuffer(&draw));assert(exports==1);export_ok=true;
    reset();draw.textures[0]=NULL;assert(!driWineHuaPresentFrontBuffer(&draw));assert(!exports);draw.textures[0]=(void*)1;
    reset();draw.flush_frontbuffer=NULL;assert(!driWineHuaPresentFrontBuffer(&draw));assert(!exports);
    return 0;
}
"""
with tempfile.TemporaryDirectory(prefix='vp-front-present-') as tmp:
    src, exe = Path(tmp)/'test.c', Path(tmp)/'test'
    src.write_text(code)
    subprocess.run([os.environ.get('CC','cc'),'-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: current drawable, driver gating, CPU queue order, one export and failure propagation')
