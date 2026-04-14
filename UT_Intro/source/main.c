#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <3ds.h>
#include <citro2d.h>

#include <assert.h>
#include <time.h>

#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define SCREEN_WIDTH  400
#define SCREEN_HEIGHT 240
#define MAX_SPRITES 50
#define TEXT_BUFFERS 16
#define TEXT_SFX_CHANNEL 1

// Simple sprite struct
typedef struct
{
	C2D_Sprite spr;
	int time; // time for sprite to stay on screen
} Sprite;

typedef struct {
	char riff[4];        // "RIFF"
	uint32_t size;
	char wave[4];        // "WAVE"
	char fmt[4];         // "fmt "
	uint32_t fmt_size;
	uint16_t format;     // 1 = PCM
	uint16_t channels;
	uint32_t sampleRate;
	uint32_t byteRate;
	uint16_t blockAlign;
	uint16_t bitsPerSample;
	char data[4];        // "data"
	uint32_t dataSize;
} WAVHeader;

static C2D_SpriteSheet spriteSheet;
static Sprite sprites[MAX_SPRITES];

C2D_TextBuf g_staticBuf;
C2D_Text g_staticText[11];
C2D_Font font[2];
double slideStartTime = 0;
double fadeDuration = 0.5;

static const char *introsongpath = "romfs:/mus_story.wav";  // Path to wav file to play
ndspWaveBuf waveBuf;
int16_t* audioData = NULL;

ndspWaveBuf textBufs[TEXT_BUFFERS];
int textBufIndex = 0;
int16_t* textAudioData = NULL;
uint32_t textSampleRate = 0;
uint32_t textDataSize = 0;

int16_t* introAudioData = NULL;
uint32_t introSampleRate = 0;
uint32_t introDataSize = 0;
ndspWaveBuf introBuf;
bool introPlayed = false;

int textCharIndex = 0;
double lastCharTime = 0;
double charDelay = 0.04;

const char* introStrings[12] = {
    "Long ago, two races\nruled over Earth:\nHUMANS and MONSTERS.",
    "One day, war broke\nout between the two\nraces.",
    "After a long battle,\nthe humans were\nvictorious.",
    "They sealed the monsters\nunderground with a magic\nspell.",
    "Many years later...",
    "MT. EBOTT\n201X",
    "Legends say that those\nwho climb the mountain\nnever return.",
    "",
    "",
    "",
    "",
    ""
};

static void initSprites() {
	for (size_t i = 0; i < 12; i++){
		Sprite* sprite = &sprites[i];
		C2D_SpriteFromSheet(&sprite->spr, spriteSheet, i);
		if(i==10) {
			C2D_SpriteSetCenter(&sprite->spr, 0.5f, 1.0f);
			C2D_SpriteSetPos(&sprite->spr, SCREEN_WIDTH/2, 138);
		} else {
			C2D_SpriteSetCenter(&sprite->spr, 0.5f, 0.5f);
			C2D_SpriteSetPos(&sprite->spr, SCREEN_WIDTH/2, SCREEN_HEIGHT/2);
		}
		sprite->time = 2;
	}
}

bool audioInitWav() {
    FILE* f = fopen(introsongpath, "rb");
    if (!f) {
        printf("Failed to open WAV file\n");
        return false;
    }

    char chunkId[4];
    uint32_t chunkSize;

    uint16_t channels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;

    // Check RIFF header
    fread(chunkId, 1, 4, f); // "RIFF"
    fread(&chunkSize, 4, 1, f);
    fread(chunkId, 1, 4, f); // "WAVE"

    if (strncmp(chunkId, "WAVE", 4) != 0) {
        printf("Not a WAV file\n");
        fclose(f);
        return false;
    }

    // Read chunks
    while (fread(chunkId, 1, 4, f) == 4) {
        fread(&chunkSize, 4, 1, f);

        if (strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t format;
            fread(&format, 2, 1, f);
            fread(&channels, 2, 1, f);
            fread(&sampleRate, 4, 1, f);

            fseek(f, 6, SEEK_CUR); // skip byteRate + blockAlign
            fread(&bitsPerSample, 2, 1, f);

            // skip rest if fmt chunk larger
            if (chunkSize > 16)
                fseek(f, chunkSize - 16, SEEK_CUR);
        }
        else if (strncmp(chunkId, "data", 4) == 0) {

            audioData = (int16_t*) linearAlloc(chunkSize);
            if (!audioData) {
                printf("Memory alloc failed\n");
                fclose(f);
                return false;
            }

            fread(audioData, 1, chunkSize, f);
            fclose(f);

            // Setup NDSP
            ndspChnReset(0);
            ndspSetOutputMode(NDSP_OUTPUT_STEREO);
            ndspChnSetInterp(0, NDSP_INTERP_POLYPHASE);
            ndspChnSetRate(0, sampleRate);

            ndspChnSetFormat(0,
                channels == 1
                ? NDSP_FORMAT_MONO_PCM16
                : NDSP_FORMAT_STEREO_PCM16);

            float mix[12] = {0};
            mix[0] = 1.0f;
            mix[1] = 1.0f;
            ndspChnSetMix(0, mix);

            memset(&waveBuf, 0, sizeof(waveBuf));
            waveBuf.data_vaddr = audioData;

            int totalSamples = chunkSize / sizeof(int16_t);
            waveBuf.nsamples = totalSamples / channels;

            waveBuf.status = NDSP_WBUF_DONE;

            DSP_FlushDataCache(audioData, chunkSize);
            ndspChnWaveBufAdd(0, &waveBuf);

            return true;
        }
        else {
            // skip unknown chunk
            fseek(f, chunkSize, SEEK_CUR);
        }
    }

    printf("No data chunk found\n");
    fclose(f);
    return false;
}

bool audioInitEffects(int select) {
	FILE* f = NULL;
    if (select == 0){
    	f = fopen("romfs:/SND_TXT2.wav", "rb");
    } else if (select == 1){
    	f = fopen("romfs:/mus_intronoise.wav", "rb");
    } 
    if (!f){ 
    	printf("Failed to open text WAV\n");
    	return false;
    }

    char chunkId[4];
    uint32_t chunkSize;
    uint16_t channels = 0;

    fread(chunkId, 1, 4, f);
    fread(&chunkSize, 4, 1, f);
    fread(chunkId, 1, 4, f);

    while (fread(chunkId, 1, 4, f) == 4) {
        fread(&chunkSize, 4, 1, f);

        if (strncmp(chunkId, "fmt ", 4) == 0) {
            uint16_t format;
            fread(&format, 2, 1, f);
            fread(&channels, 2, 1, f);

            if (select == 0)
                fread(&textSampleRate, 4, 1, f);
            else
                fread(&introSampleRate, 4, 1, f);

            fseek(f, 6, SEEK_CUR);
            uint16_t bitsPerSample;
            fread(&bitsPerSample, 2, 1, f);

            if (chunkSize > 16)
                fseek(f, chunkSize - 16, SEEK_CUR);
        }
        else if (strncmp(chunkId, "data", 4) == 0) {

            if (select == 0) {
                textDataSize = chunkSize;
                textAudioData = (int16_t*)linearAlloc(chunkSize);
                fread(textAudioData, 1, chunkSize, f);
            } else {
                introDataSize = chunkSize;
                introAudioData = (int16_t*)linearAlloc(chunkSize);
                fread(introAudioData, 1, chunkSize, f);
            }

            fclose(f);
            return true;
        }
        else {
            fseek(f, chunkSize, SEEK_CUR);
        }
    }

    fclose(f);
    return false;
}

void playTextBlip() {
    ndspWaveBuf* buf = &textBufs[textBufIndex];

    if (buf->status == NDSP_WBUF_PLAYING)
        return; // skip if still playing
    //if (buf->status == NDSP_WBUF_PLAYING)
    //	ndspChnWaveBufClear(TEXT_SFX_CHANNEL);

    float pitch = 0.95f + (rand() % 10) / 100.0f;
    ndspChnSetRate(TEXT_SFX_CHANNEL, textSampleRate * pitch);

    memset(buf, 0, sizeof(ndspWaveBuf));
    buf->data_vaddr = textAudioData;
    buf->nsamples = textDataSize / sizeof(int16_t);
    buf->status = NDSP_WBUF_DONE;

    DSP_FlushDataCache(textAudioData, textDataSize);
    ndspChnWaveBufAdd(TEXT_SFX_CHANNEL, buf);

    textBufIndex = (textBufIndex + 1) % TEXT_BUFFERS;
}

void play_introsound() {
    if (introPlayed) return; // only once

    ndspChnReset(TEXT_SFX_CHANNEL);
    ndspChnSetInterp(TEXT_SFX_CHANNEL, NDSP_INTERP_POLYPHASE);
    ndspChnSetRate(TEXT_SFX_CHANNEL, introSampleRate);
    ndspChnSetFormat(TEXT_SFX_CHANNEL, NDSP_FORMAT_STEREO_PCM16);

    float mix[12] = {0};
    mix[0] = 1.0f;
    mix[1] = 1.0f;
    ndspChnSetMix(TEXT_SFX_CHANNEL, mix);

    memset(&introBuf, 0, sizeof(ndspWaveBuf));
    introBuf.data_vaddr = introAudioData;
    introBuf.nsamples = introDataSize / sizeof(int16_t);
    introBuf.status = NDSP_WBUF_DONE;

    DSP_FlushDataCache(introAudioData, introDataSize);
    ndspChnWaveBufAdd(TEXT_SFX_CHANNEL, &introBuf);

    introPlayed = true;
}

void drawIntroTextAnimated(int index, double currentTime) {
    const char* fullText = introStrings[index];

    int len = strlen(fullText);

    if (textCharIndex < len) {
	    char c = fullText[textCharIndex];

	    double delay = charDelay;

	    if (c == '.') delay += 0.25;
		if (c == '\n') delay += 0.2;
		if (textCharIndex > len - 5) delay += 0.02;

	    if (currentTime - lastCharTime >= delay) {
	        textCharIndex++;
	        lastCharTime = currentTime;

	        //play sound
		    if (c != ' ' && c != '\n' && (rand() % 2 == 0)) {
		        playTextBlip();
		    }
	    }
	}

    if (textCharIndex > len) textCharIndex = len;

    char temp[512];
    memset(temp, 0, sizeof(temp));
    strncpy(temp, fullText, textCharIndex);

    C2D_Text text;
    C2D_TextFontParse(&text, font[0], g_staticBuf, temp);
    C2D_TextOptimize(&text);
    
    float x = (SCREEN_WIDTH/2) - 100;
    if(index==5) {
    	x += 100;
    	C2D_DrawText(&text,
	        C2D_WithColor | C2D_WordWrap | C2D_AlignCenter,
	        x,
	        (SCREEN_HEIGHT/2) + 45,
	        0.5f,
	        0.66f, 0.66f,
	        C2D_Color32(255,255,255,255),
	        250.0f,
	        0.2f
    	);
    } else {
    	C2D_DrawText(&text,
	        C2D_WithColor | C2D_WordWrap,
	        x,
	        (SCREEN_HEIGHT/2) + 45,
	        0.5f,
	        0.66f, 0.66f,
	        C2D_Color32(255,255,255,255),
	        250.0f,
	        0.2f
	    );
    }


    
}



int main(int argc, char* argv[])
{
	// Init libs
	romfsInit();
	cfguInit();
	ndspInit();
	gfxInitDefault();
	C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
	C2D_Init(C2D_DEFAULT_MAX_OBJECTS);
	C2D_Prepare();
	g_staticBuf = C2D_TextBufNew(4096);
	consoleInit(GFX_BOTTOM, NULL);

	// Create screens
	C3D_RenderTarget* top = C2D_CreateScreenTarget(GFX_TOP, GFX_LEFT);

	// Load graphics
	spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/work.t3x");
	if (!spriteSheet) svcBreak(USERBREAK_PANIC);

	font[0] = C2D_FontLoad("romfs:/DTM-Mono.bcfnt");
	font[1] = C2D_FontLoad("romfs:/DTM-Sans.bcfnt");

	// Initialize sprites
	initSprites();

	if (!audioInitWav()) {
		printf("Audio init failed\n");
	}
	if (!audioInitEffects(0)) {
		printf("Text blip init failed\n");
	}
	if (!audioInitEffects(1)) {
		printf("Text blip init failed\n");
	}

	//printf("\x1b[21;16HPress Start to exit.");

	//Get current time
	double startTime = osGetTime() / 1000.0;
	size_t introSpriteIndex = 0;
	double slideTimes[] = {
	    0.0,
	    6.0,
	    12.0,
	    17.0,
	    24.0,
	    28.0,
	    33.5,
	    40.5,
	    45.0,
	    51.0,
	    57.0,
	    76.0
	};
	//C2D_ImageTint tint;
	
	// Main loop
	while (aptMainLoop())
	{
		//gfxSwapBuffers();
		hidScanInput();

		// Your code goes here
		u32 kDown = hidKeysDown();
		if (kDown & KEY_START)
			break; // break in order to return to hbmenu

		double currentTime = osGetTime() / 1000.0 - startTime;
		int newIndex = 0;
		for (int i = 0; i < 12; i++) {
		    if (currentTime >= slideTimes[i]) {
		        newIndex = i;
		    }
		}
		if (newIndex != introSpriteIndex) {
		    introSpriteIndex = newIndex;
		    textCharIndex = 0;
		    lastCharTime = currentTime;
		    slideStartTime = currentTime;
		 
		    // Reset sprite alpha to 0 so fade-in starts correctly
		    // C2D_SpriteSetTint(&sprites[introSpriteIndex].spr,
		    //     C2D_TopLeft, C2D_Color32(255,255,255,0),
		    //     C2D_TopRight, C2D_Color32(255,255,255,0),
		    //     C2D_BottomLeft, C2D_Color32(255,255,255,0),
		    //     C2D_BottomRight, C2D_Color32(255,255,255,0)
		    // );
		}
		double elapsed = currentTime - slideStartTime;
		C2D_ImageTint tint;

		if (introSpriteIndex < 11) {

			// Default fully visible
			float alpha = 1.0f;
			// Fade in
			if (elapsed < fadeDuration) {
			    alpha = elapsed / fadeDuration;
			}

			// Fade out (before next slide)
			double nextSlideTime = 9999.0;
			for (int i = introSpriteIndex + 1; i < 11; i++) {
			    if (slideTimes[i] > slideTimes[introSpriteIndex]) {
			        nextSlideTime = slideTimes[i] - slideTimes[introSpriteIndex];
			        break;
			    }
			}
			if (nextSlideTime < 9999.0) {
			    if (elapsed > nextSlideTime - fadeDuration) {
			        double t = (elapsed - (nextSlideTime - fadeDuration)) / fadeDuration;
			        alpha = 1.0f - t;
			    }
			}
			// Clamp
			if (alpha < 0.0f) alpha = 0.0f;
			if (alpha > 1.0f) alpha = 1.0f;

			u8 a = (u8)(alpha * 255.0f);

			// Set each corner properly
			tint.corners[C2D_TopLeft] = (C2D_Tint){
			    .color = C2D_Color32(255,255,255,a),
			    .blend = 1.0f
			};

			tint.corners[C2D_TopRight] = (C2D_Tint){
			    .color = C2D_Color32(255,255,255,a),
			    .blend = 1.0f
			};

			tint.corners[C2D_BotLeft] = (C2D_Tint){
			    .color = C2D_Color32(255,255,255,a),
			    .blend = 1.0f
			};

			tint.corners[C2D_BotRight] = (C2D_Tint){
			    .color = C2D_Color32(255,255,255,a),
			    .blend = 1.0f
			};

			C2D_SetTintMode(C2D_TintMult);
		}

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
		C2D_SceneBegin(top);
		C2D_TextBufClear(g_staticBuf);
		//C2D_DrawRectSolid(0, 0, 0, 400, 240, 0xff0000cc);
		//C2D_Flush();
		
		if (introSpriteIndex == 10) {
		    //double elapsed = currentTime - slideStartTime;
		    if (elapsed > 4.0) {
		    	float scrollSpeed = 30.0f;

			    // Start with bottom of sprite aligned to window bottom
			    float baseY = 138;

			    float spriteY = baseY + ((elapsed - 4.0) * scrollSpeed);

			    // Clamp so top doesn't go past end
			    float maxScroll = 350 - 110;
			    float maxY = baseY + maxScroll;

			    if (spriteY > maxY)
			        spriteY = maxY;

			    C2D_SpriteSetPos(&sprites[10].spr, SCREEN_WIDTH / 2, spriteY);
		    }

		    // Enable clipping
		    C2D_Flush();
		    C3D_SetScissor(GPU_SCISSOR_NORMAL, 102, 10, 212, 400);

		    C2D_DrawSpriteTinted(&sprites[introSpriteIndex].spr, &tint);

		    C2D_Flush();
		    // Disable clipping
		    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		} else if (introSpriteIndex == 11){
			C2D_DrawSprite(&sprites[introSpriteIndex].spr);
			play_introsound();
		} 
		else {
			C2D_DrawSpriteTinted(&sprites[introSpriteIndex].spr, &tint);
		}
		drawIntroTextAnimated(introSpriteIndex, currentTime);
		C3D_FrameEnd(0);
	}

	// Clean up audio
	ndspChnReset(0);
    linearFree(audioData);
    linearFree(textAudioData);
    ndspExit();

	// Delete graphics
	C2D_SpriteSheetFree(spriteSheet);
	C2D_TextBufDelete(g_staticBuf);
	C2D_FontFree(font[0]);
	C2D_FontFree(font[1]);

	// Deinit libs
	C2D_Fini();
	C3D_Fini();
	gfxExit();
	romfsExit();
	return 0;
}
