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

static const char *PATH = "romfs:/mus_story.wav";  // Path to wav file to play
ndspWaveBuf waveBuf;
int16_t* audioData = NULL;

int textCharIndex = 0;
double lastCharTime = 0;
double charDelay = 0.04;

const char* introStrings[11] = {
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
    ""
};

static void initSprites() {
	for (size_t i = 0; i < 11; i++){
		Sprite* sprite = &sprites[i];
		C2D_SpriteFromSheet(&sprite->spr, spriteSheet, i);
		C2D_SpriteSetCenter(&sprite->spr, 0.5f, 0.5f);
		C2D_SpriteSetPos(&sprite->spr, SCREEN_WIDTH/2, SCREEN_HEIGHT/2);
		sprite->time = 2;
	}
}

bool audioInitWav() {
    FILE* f = fopen(PATH, "rb");
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
	    }
	}

    if (textCharIndex > len) textCharIndex = len;

    char temp[512];
    memset(temp, 0, sizeof(temp));
    strncpy(temp, fullText, textCharIndex);

    C2D_Text text;
    C2D_TextFontParse(&text, font[0], g_staticBuf, temp);
    C2D_TextOptimize(&text);

    C2D_DrawText(&text,
        C2D_WithColor | C2D_WordWrap,
        (SCREEN_WIDTH/2) - 100,
        (SCREEN_HEIGHT/2) + 45,
        0.5f,
        0.66f, 0.66f,
        C2D_Color32(255,255,255,255),
        250.0f,
        0.2f
    );
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
	spriteSheet = C2D_SpriteSheetLoad("romfs:/gfx/intro.t3x");
	if (!spriteSheet) svcBreak(USERBREAK_PANIC);

	font[0] = C2D_FontLoad("romfs:/DTM-Mono.bcfnt");
	font[1] = C2D_FontLoad("romfs:/DTM-Sans.bcfnt");

	// Initialize sprites
	initSprites();
	//initText();


	if (!audioInitWav()) {
		printf("Audio init failed\n");
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
	    57.0
	};
	
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
		for (int i = 0; i < 11; i++) {
		    if (currentTime >= slideTimes[i]) {
		        newIndex = i;
		    }
		}
		if (newIndex != introSpriteIndex) {
		    introSpriteIndex = newIndex;
		    textCharIndex = 0;
		    lastCharTime = currentTime;
		    slideStartTime = currentTime;
		}

		float startOffset = -75.0f; // tweak this
		float spriteYOffset = 0.0f;

		if (introSpriteIndex == 10) {
		    double elapsed = currentTime - slideStartTime;

		    float scrollSpeed = 30.0f; // pixels/sec (tweak)

		    spriteYOffset = startOffset + (elapsed * scrollSpeed);

		    // Optional clamp so it doesn't scroll forever
		    float maxOffset = 150.0f; // depends on sprite height
		    if (spriteYOffset > maxOffset)
		        spriteYOffset = maxOffset;

		    C2D_SpriteSetPos(&sprites[10].spr, SCREEN_WIDTH / 2, ((SCREEN_HEIGHT/2)+spriteYOffset));
		}
		

		C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
		C2D_TargetClear(top, C2D_Color32f(0.0f, 0.0f, 0.0f, 1.0f));
		C2D_SceneBegin(top);
		C2D_TextBufClear(g_staticBuf);
		//if (introSpriteIndex == 10) {
		if (true) {
		    //float windowHeight = 120.0f;
		    //float windowY = SCREEN_HEIGHT - windowHeight;
		    float windowHeight = 110.0;
			float windowY = SCREEN_HEIGHT - windowHeight; // anchored at bottom
			//float windowY = 28;


		    // Enable clipping
		    //C3D_SetScissor(GPU_SCISSOR_NORMAL, 60, 28, 260, 138);
		    C3D_SetScissor(GPU_SCISSOR_NORMAL, 60, 28, 260, 138);

		    C2D_DrawSprite(&sprites[10].spr);

		    // Disable clipping
		    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
		} else {
		    C2D_DrawSprite(&sprites[introSpriteIndex].spr);
		}
		//C2D_DrawSprite(&sprites[introSpriteIndex].spr);
		drawIntroTextAnimated(introSpriteIndex, currentTime);
		C3D_FrameEnd(0);
	}

	// Clean up audio
	ndspChnReset(0);
    linearFree(audioData);
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
