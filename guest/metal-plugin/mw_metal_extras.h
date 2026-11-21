/* SPDX-License-Identifier: MIT */
/*
 * The one piece of Metal SPI this experiment needs that Metal does not have: a way to get an
 * MTLDrawable without CAMetalLayer.
 *
 * On a real iPhone a drawable comes from CAMetalLayer's -nextDrawable: QuartzCore owns the surface
 * and hands it to the device. This port's CoreAnimation has no Metal renderer and our test program
 * is not a window client, so the device bundle itself makes the drawable: HakiosMetalDevice answers
 * -newDrawableWithTexture: (our own selector, not Metal's) with an object that conforms to
 * MTLDrawable and additionally has a -texture getter. metal_frame then does what any Metal client
 * does with a drawable: blit into that texture, presentDrawable:, commit.
 *
 * Both the bundle and the test programs include this header; nothing else in the protocol or the
 * host knows about it.
 */
#ifndef METALWIRE_GUEST_METAL_EXTRAS_H
#define METALWIRE_GUEST_METAL_EXTRAS_H

#import <Metal/Metal.h>

@protocol MWRemoteDrawable <MTLDrawable>
/* The texture this drawable presents; the same texture the client blitted into. */
@property(nonatomic, readonly) id<MTLTexture> texture;
@end

@protocol MWRemoteDevice <NSObject>
/* Makes a drawable holding `texture`; returns nil when the device or the texture is not ours. */
- (id<MWRemoteDrawable>)newDrawableWithTexture:(id<MTLTexture>)texture;
@end

#endif /* METALWIRE_GUEST_METAL_EXTRAS_H */
