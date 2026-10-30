/* Exercise the actual patched Mesa cache with deterministic transfer callbacks.
 * This checks cache lifecycle/residency; it is not an OpenGL rendering test. */
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/mman.h>
#include "sp_tex_tile_cache.c"
static unsigned tile_reads,maps,unmaps;
static float generation=7;
static struct pipe_transfer transfer;
static void* mapped(struct pipe_context* pipe,struct pipe_resource* resource,unsigned level,
                    unsigned usage,const struct pipe_box* box,struct pipe_transfer** out) {
   (void)pipe;(void)level;(void)usage;(void)box;
   ++maps;transfer.resource=resource;*out=&transfer;return &generation;
}
static void unmapped(struct pipe_context* pipe,struct pipe_transfer* value) {
   (void)pipe;assert(value==&transfer);++unmaps;
}
void pipe_get_tile_rgba(struct pipe_transfer* value,const void* map,unsigned x,unsigned y,
                       unsigned width,unsigned height,enum pipe_format format,void* output) {
   float* out=output;
   (void)x;(void)y;(void)format;assert(value==&transfer&&map==&generation);++tile_reads;
   for(unsigned i=0;i<width*height*4;i++)out[i]=generation;
}
int main(void) {
   struct pipe_context pipe={0};pipe.texture_map=mapped;pipe.texture_unmap=unmapped;
   struct pipe_resource resource={0};pipe_reference_init(&resource.reference,1);
   resource.target=PIPE_TEXTURE_2D;resource.width0=1024;resource.height0=64;resource.depth0=1;
   struct pipe_sampler_view view={0};view.texture=&resource;view.format=PIPE_FORMAT_R32G32B32A32_FLOAT;
   struct softpipe_tex_tile_cache* tc=sp_create_tex_tile_cache(&pipe);assert(tc);
   size_t page=(size_t)sysconf(_SC_PAGESIZE),pages=(sizeof(*tc)+page-1)/page;
   unsigned char resident[128];assert(pages<=sizeof(resident));
   assert(!mincore(tc,sizeof(*tc),resident));unsigned touched=0;
   for(unsigned i=0;i<pages;i++)touched+=resident[i]&1;
   assert(touched<=2); // no eager writes to the sixteen pixel tiles
   sp_tex_tile_cache_set_sampler_view(tc,&view);
   union tex_tile_address zero=tex_tile_address(0,0,0,0,0);
   const struct softpipe_tex_cached_tile* tile=sp_get_cached_tile_tex(tc,zero);
   assert(tile->data.color[0][0][0]==7&&tile_reads==1&&maps==1);
   assert(sp_get_cached_tile_tex(tc,zero)==tile&&tile_reads==1);
   generation=8;sp_tex_tile_cache_validate_texture(tc);
   assert(sp_get_cached_tile_tex(tc,zero)->data.color[0][0][0]==8&&tile_reads==2);
   union tex_tile_address collision=tex_tile_address(16*TEX_TILE_SIZE,0,0,0,0);
   assert(tex_cache_pos(collision)==tex_cache_pos(zero));
   generation=9;assert(sp_get_cached_tile_tex(tc,collision)->data.color[0][0][0]==9&&tile_reads==3);
   generation=10;assert(sp_get_cached_tile_tex(tc,zero)->data.color[0][0][0]==10&&tile_reads==4);
   sp_flush_tex_tile_cache(tc);generation=11;
   assert(sp_get_cached_tile_tex(tc,zero)->data.color[0][0][0]==11&&tile_reads==5);
   sp_tex_tile_cache_set_sampler_view(tc,NULL);
   sp_tex_tile_cache_set_sampler_view(tc,&view);generation=12;
   assert(sp_get_cached_tile_tex(tc,zero)->data.color[0][0][0]==12&&tile_reads==6&&maps==3&&unmaps==2);
   sp_tex_tile_cache_set_sampler_view(tc,NULL);sp_destroy_tex_tile_cache(tc);
   assert(unmaps==maps&&resource.reference.count==1);
   printf("Softpipe actual cache: address zero, hits, invalidation, collision, rebind and release passed; unused resident=%u/%zu pages\n",touched,pages);
   return 0;
}
