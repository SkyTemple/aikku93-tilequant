/**************************************/
#include <stdlib.h>
/**************************************/
#include "Bitmap.h"
#include "Colourspace.h"
#include "Qualetize.h"
#include "Tiles.h"
/**************************************/

//! Dither modes available
#define DITHER_NONE           ( 0) //! No dither
#define DITHER_ORDERED(n)     ( n) //! Ordered dithering (Kernel size: (2^n) x (2^n))
#define DITHER_FLOYDSTEINBERG (-1) //! Floyd-Steinberg (diffusion)

//! Dither settings
//! NOTE: Ordered dithering gives consistent tiled results, but Floyd-Steinberg can look nicer.
//!       Recommend dither level of 0.5 for ordered, and 1.0 for Floyd-Steinberg.
#define DITHER_LEVEL 0.5f
#define DITHER_TYPE  DITHER_ORDERED(3)

//! When not zero, the PSNR for each channel will be displayed
#define MEASURE_PSNR 1

/**************************************/

//! Palette entry matching
static int FindPaletteEntry(const struct BGRAf_t *Px, const struct BGRAf_t *Pal, int MaxPalSize, int PalUnused) {
	int   i;
	int   MinIdx = 0;
	float MinDst = 8.0e37f; //! Arbitrarily large number
	for(i=PalUnused-1;i<MaxPalSize;i++) {
		float Dst = BGRAf_ColDistance(Px, &Pal[i]);
		if(Dst < MinDst) MinIdx = i, MinDst = Dst;
	}
	return MinIdx;
}

/**************************************/

//! Handle conversion of image with given palette, return RMS error
//! NOTE: Lots of pointer aliasing to avoid even more memory consumption
struct BGRAf_t Qualetize(
	struct BmpCtx_t *Image,
	struct TilesData_t *TilesData,
	uint8_t *PxData,
	struct BGRAf_t *Palette,
	int MaxTilePals,
	int MaxPalSize,
	int PalUnused,
	int ReplaceImage
) {
	int i;

	//! Do processing
	TilesData_QuantizePalettes(TilesData, Palette, MaxTilePals, MaxPalSize, PalUnused);

	//! Convert pixels to palettized
	int x, y;
	int ImgW = Image->Width;
	int ImgH = Image->Height;
	int TileW = TilesData->TileW;
	int TileH = TilesData->TileH;
	int32_t *TilePalIdx = TilesData->TilePalIdx;
#if MEASURE_PSNR
	      struct BGRAf_t  RMSE     = (struct BGRAf_t){0,0,0,0};
#endif
	const        uint8_t *PxSrcIdx = Image->ColPal ? Image->PxIdx  : NULL;
	const struct BGRA8_t *PxSrcBGR = Image->ColPal ? Image->ColPal : Image->PxBGR;
#if DITHER_TYPE != DITHER_NONE
# if DITHER_TYPE == DITHER_FLOYDSTEINBERG
	struct BGRAf_t *PxDiffuse = TilesData->PxData;
	for(y=0;y<ImgH;y++) for(x=0;x<ImgW;x++) PxDiffuse[y*ImgW+x] = (struct BGRAf_t){0,0,0,0};
# else
	struct BGRAf_t *PaletteSpread = TilesData->PxData;
	for(i=0;i<MaxTilePals;i++) {
		//! Find the mean values of this palette
		int n;
		struct BGRAf_t Mean = (struct BGRAf_t){0,0,0,0};
		for(n=PalUnused;n<MaxPalSize;n++) Mean = BGRAf_Add(&Mean, &Palette[i*MaxPalSize+n]);
		Mean = BGRAf_Divi(&Mean, MaxPalSize-PalUnused);

		//! Compute the average slopes and store to the palette spread
		struct BGRAf_t Spread = (struct BGRAf_t){0,0,0,0};
		for(n=PalUnused;n<MaxPalSize;n++) {
			struct BGRAf_t d = BGRAf_Sub(&Palette[i*MaxPalSize+n], &Mean);
			d = BGRAf_Abs(&d);
			Spread = BGRAf_Add(&Spread, &d);
		}
		PaletteSpread[i] = BGRAf_Divi(&Spread, MaxPalSize-PalUnused);
	}
# endif
#endif
	for(y=0;y<ImgH;y++) for(x=0;x<ImgW;x++) {
		int PalIdx = TilePalIdx[(y/TileH)*(ImgW/TileW) + (x/TileW)];

		//! Get original pixel data
		struct BGRAf_t Px, Px_Original; {
			struct BGRA8_t p;
			if(PxSrcIdx) p = PxSrcBGR[PxSrcIdx[y*ImgW + x]];
			else         p = PxSrcBGR[         y*ImgW + x ];
			Px_Original = BGRAf_FromBGRA8(&p);
			Px_Original = BGRAf_AsYCoCg(&Px_Original);
			Px = Px_Original;
		}
#if DITHER_TYPE != DITHER_NONE
# if DITHER_TYPE == DITHER_FLOYDSTEINBERG
		//! Adjust for diffusion error
		{
			struct BGRAf_t Dif = PxDiffuse[y*ImgW + x];
			Dif = BGRAf_Muli(&Dif, DITHER_LEVEL);
			Px  = BGRAf_Add (&Px, &Dif);
		}
# else
		//! Adjust for dither matrix
		{
			int Threshold = 0, xKey = x, yKey = x^y;
			int Bit = DITHER_TYPE-1; do {
				Threshold = Threshold*2 + (yKey & 1), yKey >>= 1; //! <- Hopefully turned into "SHR, ADC"
				Threshold = Threshold*2 + (xKey & 1), xKey >>= 1;
			} while(--Bit >= 0);
			float fThres = Threshold * (1.0f / (1 << (2*DITHER_TYPE))) - 0.5f;
			struct BGRAf_t DitherVal = BGRAf_Muli(&PaletteSpread[PalIdx], fThres*DITHER_LEVEL);
			Px = BGRAf_Add(&Px, &DitherVal);
		}
# endif
#endif
		//! Find matching palette entry and store
		int PalCol = FindPaletteEntry(&Px, Palette + PalIdx*MaxPalSize, MaxPalSize, PalUnused);
		PxData[y*ImgW + x] = PalIdx*MaxPalSize + PalCol;
#if MEASURE_PSNR || DITHER_TYPE == DITHER_FLOYDSTEINBERG
		struct BGRAf_t Error = BGRAf_Sub(&Px_Original, &Palette[PxData[y*ImgW + x]]);
#endif
#if DITHER_TYPE == DITHER_FLOYDSTEINBERG
		//! Store error diffusion
		if(y+1 < ImgH) {
			if(x > 0) {
				struct BGRAf_t t = BGRAf_Muli(&Error, 3.0f/16);
				PxDiffuse[(y+1)*ImgW+(x-1)] = BGRAf_Add(&PxDiffuse[(y+1)*ImgW+(x-1)], &t);
			}
			if(1) {
				struct BGRAf_t t = BGRAf_Muli(&Error, 5.0f/16);
				PxDiffuse[(y+1)*ImgW+(x  )] = BGRAf_Add(&PxDiffuse[(y+1)*ImgW+(x  )], &t);
			}
			if(x+1 < ImgW) {
				struct BGRAf_t t = BGRAf_Muli(&Error, 1.0f/16);
				PxDiffuse[(y+1)*ImgW+(x+1)] = BGRAf_Add(&PxDiffuse[(y+1)*ImgW+(x+1)], &t);
			}
		}
		if(x+1 < ImgW) {
				struct BGRAf_t t = BGRAf_Muli(&Error, 7.0f/16);
				PxDiffuse[(y  )*ImgW+(x+1)] = BGRAf_Add(&PxDiffuse[(y  )*ImgW+(x+1)], &t);
		}
#endif
#if MEASURE_PSNR
		//! Accumulate squared error
		Error = BGRAf_Mul(&Error, &Error);
		RMSE  = BGRAf_Add(&RMSE, &Error);
#endif
	}

	//! Store the final palette
	//! NOTE: This aliases over the original palette, but is
	//! safe because BGRA8_t is smaller than BGRAf_t
	struct BGRA8_t *PalBGR = (struct BGRA8_t*)Palette;
	for(i=0;i<BMP_PALETTE_COLOURS;i++) {
		struct BGRAf_t x = BGRAf_FromYCoCg(&Palette[i]);
		PalBGR[i] = BGRA8_FromBGRAf(&x);
	}

	//! Store new image data
	if(ReplaceImage) {
		if(Image->ColPal) {
			free(Image->ColPal);
			free(Image->PxIdx);
		} else free(Image->PxBGR);
		Image->ColPal = PalBGR;
		Image->PxIdx  = PxData;
	}

	//! Return error
#if MEASURE_PSNR
	RMSE = BGRAf_Divi(&RMSE, ImgW*ImgH);
	RMSE = BGRAf_Sqrt(&RMSE);
	return RMSE;
#else
	return (struct BGRAf_t){0,0,0,0};
#endif
}

/**************************************/
//! EOF
/**************************************/
