/*
 * Copyright 2009, Google Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */


// THis file contains the declaration for the Renderer class.

#ifndef O3D_CORE_CROSS_RENDERER_H_
#define O3D_CORE_CROSS_RENDERER_H_

#include <utility>
#include <vector>

#include "core/cross/bitmap.h"
#include "core/cross/buffer.h"
#include "core/cross/callback.h"
#include "core/cross/display_mode.h"
#include "core/cross/display_window.h"
#include "core/cross/draw_element.h"
#include "core/cross/effect.h"
#include "core/cross/lost_resource_callback.h"
#include "core/cross/primitive.h"
#include "core/cross/sampler.h"
#include "core/cross/service_dependency.h"
#include "core/cross/service_implementation.h"
#include "core/cross/shape.h"
#include "core/cross/state.h"
#include "core/cross/texture.h"
#include "core/cross/types.h"
#include "core/cross/vector_map.h"

namespace o3d {

class Material;
class RenderSurface;
class Features;

// The Renderer class provides the abstract interface for the draw calls
// that need to be implemented for each platform. Renderer objects can be
// created independently of the Client object that uses them.
//
// The creation and deletion order of an o3d::Client using a
// o3d::Renderer object should be:
//
//     renderer = Renderer::CreateDefaultRenderer(&service_locator_);
//
//     // PlatformInit isn't a Renderer API -- You define a function
//     // for your platform to return an appropriate display window.
//     DisplayWindow* display = PlatformInit();
//
//     if (renderer->Init(display, true)) {
//       client = new Client();
//       client->Init();
//
//       ...
//
//       delete client;  // unbinds renderer from client
//       renderer->Destroy();  // deletes graphics contexts
//       delete renderer;
//     }

class Renderer {
 public:
  static const InterfaceId kInterfaceId;

  // These are in order of best to worst except for UNINITIALIZED which is
  // zero on purpose.
  //
  // Note: Do not change the values of these constants as they can be hard coded
  // in javascript. You can update them in o3djs/util.js but if you change them
  // you'll potentially break any app that is not using o3djs/util.js.
  enum InitStatus {
    UNINITIALIZED,
    SUCCESS,
    OUT_OF_RESOURCES,
    GPU_NOT_UP_TO_SPEC,
    INITIALIZATION_ERROR,
  };

  // This is exposed to JavaScript, but as long as users always refer to it
  // symbolically, it should be possible to change it without breaking anyone.
  // NOTE: windows d3d display modes are internally implemented via adding 1 to
  // their normal values of [0, NUM) so as not to collide with this value.
  enum DisplayModes {
    DISPLAY_MODE_DEFAULT = 0
  };

  // A StateHandler takes a param and sets or resets a render state.
  class StateHandler {
   public:
    StateHandler() : index_(-1) { }
    virtual ~StateHandler() { }  // Needed because we are a base class.

    // Gets Class of State's Parameter
    virtual const ObjectBase::Class* GetClass() const = 0;

    // Sets the state to the value of the param.
    // Parameters:
    //   renderer: the renderer
    //   param: param with state data
    virtual void SetState(Renderer* renderer, Param* param) const = 0;

    // Returns the index of this state handler.
    int index() const {
      return index_;
    }

    // Sets the index of this state handler.
    void set_index(int index) {
      DLOG_ASSERT(index_ < 0);  // Can only be set once.
      index_ = index;
    }

   private:
    int index_;
  };

  virtual ~Renderer();

  // Creates a 'default' renderer, choosing the correct implementation type.
  static Renderer* CreateDefaultRenderer(ServiceLocator* service_locator);

  // Gets whether or not the renderer should attempt to use the software
  // renderer.
  static bool IsForceSoftwareRenderer();

  // Initialises the renderer for use, claiming hardware resources.
  InitStatus Init(const DisplayWindow& display, bool off_screen);

  // The platform specific part of initalization.
  virtual InitStatus InitPlatformSpecific(const DisplayWindow& display,
                                          bool off_screen) = 0;

  // Initializes stuff that has to happen after Init
  virtual void InitCommon();

  // Frees anything related to the client and clears the client.
  virtual void UninitCommon();

  // Releases all hardware resources. This should be called before destroying
  // the window used for rendering. It will be called automatically from the
  // destructor.
  // Destroy() should be called before Init() is called again.
  virtual void Destroy() = 0;

  // Prepares the rendering device for subsequent draw calls.
  // This is called during a RenderTree and can be called mutliple times
  // during a single frame.
  // NOTE: See StartRendering!
  bool BeginDraw();

  // This is called during a RenderTree and can be called mutliple times during
  // a single frame. It gives the renderer a chance to flush state before
  // returning to JavaScript if JavaScript is calling RenderTree directly.
  // NOTE: See StartRendering!
  void EndDraw();

  // Does any pre-rendering preparation.
  // The order of operations is
  //   StartRendering()
  //     BeginDraw();
  //     EndDraw()
  //     BeginDraw();
  //     EndDraw()
  //     BeginDraw();
  //     EndDraw()
  //   FinishRendering();  // <- Presents the results.
  bool StartRendering();

  // Presents the results of the draw calls for this frame.
  void FinishRendering();

  // Copy the contents of the backbuffer to the window.
  void Present();

  // Returns whether a render is required.
  bool need_to_render() const {
    return need_to_render_;
  }

  // Invalidate the last rendered frame.
  void set_need_to_render(bool need_to_render) {
    need_to_render_ = need_to_render;
  }

  // Handles the plugin resize event.
  virtual void Resize(int width, int height) = 0;

  // Turns fullscreen display on.
  // Parameters:
  //  display: a platform-specific display identifier
  //  mode_id: a mode returned by GetDisplayModes
  // Returns true on success, false on failure.
  virtual bool GoFullscreen(const DisplayWindow& display,
                            int mode_id) = 0;

  // Cancels fullscreen display. Restores rendering to windowed mode
  // with the given width and height.
  // Parameters:
  //  display: a platform-specific display identifier
  //  width: the width to which to restore windowed rendering
  //  height: the height to which to restore windowed rendering
  // Returns true on success, false on failure.
  virtual bool CancelFullscreen(const DisplayWindow& display,
                                int width, int height) = 0;

  // Tells whether we're currently displayed fullscreen or not.
  virtual bool fullscreen() const = 0;

  // Get a vector of the available fullscreen display modes.
  // Clears *modes on error.
  virtual void GetDisplayModes(std::vector<DisplayMode> *modes) = 0;

  // Get a single fullscreen display mode by id.
  // Returns true on success, false on error.
  virtual bool GetDisplayMode(int id, DisplayMode *mode) = 0;

  // Gets the viewport.
  void GetViewport(Float4* rectangle, Float2* depth_range);

  // Sets the viewport.
  // Parameters:
  //   rectangle: The position and size to set the viewport in
  //       Float4(left, top, width, height) format. The default values are (0.0,
  //       0.0, 1.0, 1.0). In other words, the full area. The viewport provides
  //       the mapping of the clip space coordinates into normalized screen
  //       coordinates.
  //   depth_range:
  //      The min Z and max Z depth range in Float2(min Z, max Z) format. The
  //      default values are (0.0, 1.0). The depth range provides the mapping of
  //      the clip space coordinates into normalized z buffer coordinates.
  //
  // Note: The rectangle values must describe a rectangle that is 100% inside
  // the client area. In other words, (0.5, 0.0, 1.0, 1.0) would describe an
  // area that is 1/2 off right side of the screen. That is an invalid value and
  // will be clipped to (0.5, 0.0, 0.5, 1.0).
  void SetViewport(const Float4& rectangle, const Float2& depth_range);

  // Clears the current buffers.
  void Clear(const Float4 &color,
             bool color_flag,
             float depth,
             bool depth_flag,
             int stencil,
             bool stencil_flag);

  // Renders this Element using the parameters from override first, followed by
  // the draw_element, followed by params on this Primitive and material.
  // Parameters:
  //   element: Element to draw
  //   draw_element: DrawElement to override params with.
  //   material: Material to render with.
  //   override: Override to render with.
  void RenderElement(Element* element,
                     DrawElement* draw_element,
                     Material* material,
                     ParamObject* override,
                     ParamCache* param_cache);

  // Pushes rendering states.
  void PushRenderStates(State *state);

  // Pops rendering states to back to their previous settings.
  void PopRenderStates();

  // Binds the passed surfaces to the color and depth buffers of the
  // renderer.
  // Parameters:
  //   surface: RenderSurface to bind to the color buffer.
  //   depth_surface: RenderDepthStencilSurface to bind to the depth/stencil
  //       buffer.
  //   is_back_buffer: True if the render surface being set should be considered
  //       the backbuffer.
  void SetRenderSurfaces(const RenderSurface* surface,
                         const RenderDepthStencilSurface* depth_surface,
                         bool is_back_buffer);

  // Gets the current render surfaces.
  // Parameters:
  //   surface: pointer to variable to hold RenderSurface to bind to the color
  //       buffer.
  //   depth_surface: pointer to variable to hold RenderDepthStencilSurface to
  //       bind to the depth/stencil buffer.
  //   is_back_buffer: pointer to variable to hold whether or not the surface
  //       returned is the back buffer.
  void GetRenderSurfaces(const RenderSurface** surface,
                         const RenderDepthStencilSurface** depth_surface,
                         bool* is_back_buffer);

  // Creates a StreamBank, returning a platform specific implementation class.
  virtual StreamBank::Ref CreateStreamBank() = 0;

  // Creates a Primitive, returning a platform specific implementation class.
  virtual Primitive::Ref CreatePrimitive() = 0;

  // Creates a DrawElement, returning a platform specific implementation
  // class.
  virtual DrawElement::Ref CreateDrawElement() = 0;

  // Creates and returns a platform specific float buffer
  virtual VertexBuffer::Ref CreateVertexBuffer() = 0;

  // Creates and returns a platform specific integer buffer
  virtual IndexBuffer::Ref CreateIndexBuffer() = 0;

  // Creates and returns a platform specific effect object
  virtual Effect::Ref CreateEffect() = 0;

  // Creates and returns a platform specific Sampler object.
  virtual Sampler::Ref CreateSampler() = 0;

  // Creates and returns a ParamCache object
  ParamCache* CreateParamCache();

  // Frees a Param Cache.
  void FreeParamCache(ParamCache* param_cache);

  // Creates and returns a platform specific Texture2D object.  It allocates
  // the necessary resources to store texture data for the given image size
  // and format.
  Texture2D::Ref CreateTexture2D(int width,
                                 int height,
                                 Texture::Format format,
                                 int levels,
                                 bool enable_render_surfaces);

  // Creates and returns a platform specific TextureCUBE object.  It allocates
  // the necessary resources to store texture data for the given image size
  // and format.
  TextureCUBE::Ref CreateTextureCUBE(int edge_length,
                                     Texture::Format format,
                                     int levels,
                                     bool enable_render_surfaces);

  // Sets the lost resources callback.
  // NOTE: The client takes ownership of the LostResourcesCallback you pass in.
  // It will be deleted if you call SetLostResourcesCallback a second time or if
  // you call ClearLostResourcesCallback
  //
  // Parameters:
  //   callback: LostResourcesCallback to call each frame.
  void SetLostResourcesCallback(LostResourcesCallback* callback);

  // Clears the lost resources callback
  // NOTE: The client takes ownership of the LostResourcesCallback you pass in
  // to SetLostResourcesCallback. It will be deleted if you call
  // SetLostResourcesCallback a second time or if you call
  // ClearLostResourcesCallback.
  void ClearLostResourcesCallback();

  // Creates and returns a platform-specific RenderDepthStencilSurface object
  // for use as a depth-stencil render target.
  virtual RenderDepthStencilSurface::Ref CreateDepthStencilSurface(
      int width,
      int height) = 0;

  ServiceLocator* service_locator() const { return service_locator_; }

  // Returns the type of Param needed for a particular state.
  const ObjectBase::Class* GetStateParamType(const String& state_name) const;

  // Whether we are currently rendering (between StartRendering /
  // FinishRendering calls).
  // NOTE: See StartRendering!
  bool rendering() const {
    return rendering_;
  }

  // Whether we are currently drawing (between BeginDraw / EndDraw calls).
  // NOTE: See StartRendering!
  bool drawing() const {
    return drawing_;
  }

  // Get the client area's width.
  int width() const {
    return width_;
  }

  // Get the client area's height.
  int height() const {
    return height_;
  }

  // Get the width of the buffer to which the renderer is drawing.
  int display_width() const {
    return display_width_;
  }

  // Get the height of the buffer to which the renderer is drawing.
  int display_height() const {
    return display_height_;
  }

  // Whether or not the underlying API (D3D or OpenGL) supports
  // non-power-of-two textures.
  bool supports_npot() const {
    return supports_npot_;
  }

  // Gets the number of times we've rendered a frame.
  int render_frame_count() const {
    return render_frame_count_;
  }

  int transforms_processed() const {
    return transforms_processed_;
  }

  int transforms_culled() const {
    return transforms_culled_;
  }

  int draw_elements_processed() const {
    return draw_elements_processed_;
  }

  int draw_elements_culled() const {
    return draw_elements_culled_;
  }

  int draw_elements_rendered() const {
    return draw_elements_rendered_;
  }

  int primitives_rendered() const {
    return primitives_rendered_;
  }

  void IncrementTransformsProcessed() {
    ++transforms_processed_;
  }

  void IncrementTransformsCulled() {
    ++transforms_culled_;
  }

  void IncrementDrawElementsProcessed() {
    ++draw_elements_processed_;
  }

  void IncrementDrawElementsCulled() {
    ++draw_elements_culled_;
  }

  void IncrementDrawElementsRendered() {
    ++draw_elements_rendered_;
  }

  void AddPrimitivesRendered(int amount_to_add) {
    primitives_rendered_ += amount_to_add;
  }

  Sampler* error_sampler() const {
    return error_sampler_.Get();
  }

  Texture* error_texture() const {
    return error_texture_.Get();
  }

  Texture* fallback_error_texture() const {
    return fallback_error_texture_.Get();
  }

  ParamSampler* error_param_sampler() const {
    return error_param_sampler_.Get();
  }

  // Sets the texture use when a texture is missing. May be NULL.
  void SetErrorTexture(Texture* texture);

  // Determine if the Texture argument is safe to use in an effect.
  // If a RenderSurface contained within the texture is currently bound to the
  // renderer, then it is not safe to bind the texture.
  bool SafeToBindTexture(Texture* texture) const;

  // When rendering only part of the view because of scrolling or the window
  // being smaller than the client size, etc, this lets us adjust the origin of
  // the top left of the drawing within our area, effectively allowing us to
  // scroll within that area. Vars dest_x_offset_ and dest_y_offset_ will be 0
  // in the unclipped case, positive numbers if we are clipping the left or the
  // top respectively.  Only currently used on Mac, only currently respected by
  // the GL renderer.
  void SetClientOriginOffset(int x, int y) {
    dest_x_offset_ = x;
    dest_y_offset_ = y;
  }

  // Returns a platform specific 4 element swizzle table for RGBA UByteN fields.
  // The should contain the index of R, G, B, and A in that order for the
  // current platform.
  virtual const int* GetRGBAUByteNSwizzleTable() = 0;

  // Used only for unit testing purposes. Should not be used elsewhere.
  void set_rendering(bool rendering) {
    rendering_ = rendering;
  }

  // Used only by the ColorWriteEnable state handlers. Should not be used
  // elsewhere.
  // Sets the write mask. This must be called by platform specific renderers
  // when the color write mask is changed.
  void SetWriteMask(int mask) {
    write_mask_ = mask & 0xF;
  }

  // Indicates whether this Renderer has yet presented to the screen.
  bool presented_once() {
    return presented_once_;
  }

 protected:
  typedef vector_map<String, StateHandler*> StateHandlerMap;
  typedef std::vector<ParamVector> ParamVectorArray;
  typedef std::vector<State*> StateArray;

  // Make the constructor protected so that users can only create one
  // using the CreateDefaultRenderer factory method.
  explicit Renderer(ServiceLocator* service_locator);

  // Sets whether or not the renderer supports non-power of 2 textures.
  void SetSupportsNPOT(bool supports_npot);

  // Adds a state handler to the state handler map
  // Parameters:
  //   state_name: Name of the state.
  //   state_handler: a concrete state handler.
  void AddStateHandler(const String& state_name, StateHandler* state_handler);

  // Gets a state handler based on a Param
  // Parameters:
  //   param: Param who's name matches a state.
  // Returns:
  //   StateHandler of matching state.
  const StateHandler* GetStateHandler(Param* param) const;

  // Resets all states to their defaults.
  void SetInitialStates();

  // Returns true if the renderer is presently drawing to a RenderSurface,
  // false if the renderer is drawing to the client area.
  bool RenderSurfaceActive() const {
    return current_render_surface_ != NULL;
  }

  // Sets rendering to the back buffer.
  virtual void SetBackBufferPlatformSpecific() = 0;

  // Sets the render surfaces on a specific platform.
  virtual void SetRenderSurfacesPlatformSpecific(
      const RenderSurface* surface,
      const RenderDepthStencilSurface* depth_surface) = 0;

  // Creates a platform specific ParamCache.
  virtual ParamCache* CreatePlatformSpecificParamCache() = 0;

  // Platform specific version of CreateTexture2D
  virtual Texture2D::Ref CreatePlatformSpecificTexture2D(
      int width,
      int height,
      Texture::Format format,
      int levels,
      bool enable_render_surfaces) = 0;

  // Platform specific version of CreateTextureCUBE.
  virtual TextureCUBE::Ref CreatePlatformSpecificTextureCUBE(
      int edge_length,
      Texture::Format format,
      int levels,
      bool enable_render_surfaces) = 0;

  // The platform specific part of BeginDraw.
  virtual bool PlatformSpecificBeginDraw() = 0;

  // The platform specific part of EndDraw.
  virtual void PlatformSpecificEndDraw() = 0;

  // The platform specific part of StartRendering.
  virtual bool PlatformSpecificStartRendering() = 0;

  // The platform specific part of EndRendering.
  virtual void PlatformSpecificFinishRendering() = 0;

  // The platform specific part of Present.
  virtual void PlatformSpecificPresent() = 0;

  // The platform specific part of Clear.
  virtual void PlatformSpecificClear(const Float4 &color,
                                     bool color_flag,
                                     float depth,
                                     bool depth_flag,
                                     int stencil,
                                     bool stencil_flag) = 0;

  // Applies states that have been modified (marked dirty).
  virtual void ApplyDirtyStates() = 0;

  // Sets the viewport. This is the platform specific version.
  virtual void SetViewportInPixels(int left,
                                   int top,
                                   int width,
                                   int height,
                                   float min_z,
                                   float max_z) = 0;

  // Sets the client's size. Derived classes must call this on Init and Resize.
  void SetClientSize(int width, int height);

  // Calls any registered lost resources callback.
  void CallLostResourcesCallback() const {
    lost_resources_callback_manager_.Run();
  }

  int dest_x_offset() const {
    return dest_x_offset_;
  }

  int dest_y_offset() const {
    return dest_y_offset_;
  }

  Features* features() const {
    return features_.Get();
  }

 private:
  // Adds the default states to their respective stacks.
  void AddDefaultStates();

  // Removes the default states from their respective stacks.
  void RemoveDefaultStates();

  // Clears the backbuffer.
  void ClearBackBuffer();

  // Clears the backbuffer if it has not been cleared.
  void ClearBackBufferIfNotCleared() {
    if (!back_buffer_cleared_ && current_render_surface_is_back_buffer_) {
      ClearBackBuffer();
    }
  }

  ServiceLocator* service_locator_;
  ServiceImplementation<Renderer> service_;
  ServiceDependency<Features> features_;

  // The current render surfaces. NULL = no surface.
  const RenderSurface* current_render_surface_;
  const RenderDepthStencilSurface* current_depth_surface_;
  bool current_render_surface_is_back_buffer_;

  Sampler::Ref error_sampler_;  // sampler used when one is missing.
  Texture::Ref error_texture_;  // texture used when one is missing.
  Texture::Ref fallback_error_texture_;  // texture used when error_texture is
                                         // null.
  ParamObject::Ref error_object_;  // holds params used for missing textures.
  ParamSampler::Ref error_param_sampler_;  // A Param for the error sampler.

  // Map of State Handlers.
  StateHandlerMap state_handler_map_;

  // Stack of state params
  ParamVectorArray state_param_stacks_;

  // Stack of state objects.
  StateArray state_stack_;

  // State object holding the default state settings.
  State::Ref default_state_;

  // A State object holding the settings required to be able to clear the
  // back buffer.
  State::Ref clear_back_buffer_state_;

  // Lost Resources Callbacks.
  LostResourcesCallbackManager lost_resources_callback_manager_;

  // Current viewport setting.
  Float4 viewport_;

  // Current depth range.
  Float2 depth_range_;

  // Current write mask.
  int write_mask_;

  int render_frame_count_;  // count of times we've rendered frame.
  int transforms_processed_;  // count of transforms processed this frame.
  int transforms_culled_;  // count of transforms culled this frame.
  int draw_elements_processed_;  // count of draw elements processed this frame.
  int draw_elements_culled_;  // count of draw elements culled this frame.
  int draw_elements_rendered_;  // count of draw elements culled this frame.
  int primitives_rendered_;  // count of primitives (tris, lines)
                             // rendered this frame.

  // The depth of times we've called StartRendering/FinishRenderering.
  int start_depth_;

  // Whether we need to clear the entire client area next render.
  bool clear_client_;

  // Whether a render is required.
  bool need_to_render_;

  // Whether we are currently rendering (between StartRendering /
  // FinishRendering calls)
  bool rendering_;

  // Whether or not we are drawing (between BeingDraw/EndDraw calls)
  bool drawing_;

  int width_;  // width of the client area in pixels
  int height_;  // height of the client area in pixels

  int display_width_;  // width of the thing we are rendering to.
  int display_height_;  // height of the thing we are rendering to.

  // X and Y offsets for destination rectangle.
  int dest_x_offset_;
  int dest_y_offset_;

  // Whether or not the underlying API supports non-power-of-two textures.
  bool supports_npot_;

  // Whether the backbuffer has been cleared this frame.
  bool back_buffer_cleared_;

  // Whether we have ever completed a call to Present().
  bool presented_once_;

  DISALLOW_COPY_AND_ASSIGN(Renderer);
};

}  // namespace o3d

#endif  // O3D_CORE_CROSS_RENDERER_H_
