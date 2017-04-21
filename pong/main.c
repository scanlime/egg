/*
 * PongOS v2.5
 *
 * M. Elizabeth Scott <micah@vmware.com>
 *
 * Copyright (C) 2008-2009 VMware, Inc. Licensed under the MIT
 * License, please see the README.txt. All rights reserved.
 */

#include "svga.h"
#include "intr.h"
#include "io.h"
#include "timer.h"
#include "keyboard.h"
#include "vmbackdoor.h"
#include "math.h"
#include "vmware-logo.h"

#define PONG_DOT_SIZE           8
#define PONG_DIGIT_PIXEL_SIZE   10
#define PONG_BG_COLOR           0x000000
#define PONG_SPRITE_COLOR       0xFFFFFF
#define PONG_PLAYFIELD_COLOR    0xAAAAAA
#define PONG_FRAME_RATE         60

#define MODE_WIDTH              800
#define MODE_HEIGHT             600

#define LOGO_MARGIN             30
#define PONG_DIVIDER_HEIGHT     (66 * PONG_DOT_SIZE)
#define LOGO_X                  ((MODE_WIDTH - LOGO_WIDTH) / 2)
#define LOGO_Y                  (MODE_HEIGHT - LOGO_HEIGHT - LOGO_MARGIN)

#define MAX_DIRTY_RECTS         128
#define MAX_SPRITES             8

typedef struct {
   float x, y;
} Vector2;

typedef struct {
   int x, y, w, h;
} Rect;

typedef struct {
   Rect r;
   uint32 color;
} FillRect;

static struct {
   uint32 *buffer;
   Rect dirtyRects[MAX_DIRTY_RECTS];
   uint32 numDirtyRects;
} back;

static struct {
   FillRect paddles[2];
   FillRect ball;
   uint8 scores[2];

   float ballSpeed;
   float paddleVelocities[2];
   float paddlePos[2];
   Vector2 ballVelocity;
   Vector2 ballPos;

   Bool playfieldDirty;
   Bool drawingMode;
} pong;


/*
 *-----------------------------------------------------------------------------
 *
 * Random32 --
 *
 *    "Random" number generator. To save code space, we actually just use
 *    the low bits of the TSC. This of course isn't actually random, but
 *    it's good enough for Pong.
 *
 *-----------------------------------------------------------------------------
 */

static uint32
Random32(void)
{
   uint64 t;

   __asm__ __volatile__("rdtsc" : "=A" (t));

   return (uint32)t;
}


/*
 *-----------------------------------------------------------------------------
 *
 * RectTestIntersection --
 *
 *    Returns TRUE iff two Rects intersect with each other.
 *
 *-----------------------------------------------------------------------------
 */

static Bool
RectTestIntersection(Rect *a,  // IN
                     Rect *b)  // IN
{
   return !(a->x + a->w < b->x ||
            a->x > b->x + b->w ||
            a->y + a->h < b->y ||
            a->y > b->y + b->h);
}


/*
 *-----------------------------------------------------------------------------
 *
 * BackFill --
 *
 *    Perform a color fill on the backbuffer.
 *
 *-----------------------------------------------------------------------------
 */

static void
BackFill(FillRect fr)   // IN
{
   int i, j;

   // Default colormap, for indexed drawing modes
   static const uint32 cTab[] =  { 0x60bb44, 0xfcb827, 0xf6821f, 0xdf3a3e, 0x963d97, 0x029ddd };

   for (i = 0; i < fr.r.h; i++) {
      uint32 lineIndex = fr.r.y + i;
      uint32 *line = &back.buffer[lineIndex * MODE_WIDTH + fr.r.x];

      if (pong.drawingMode && fr.color) {
	fr.color = cTab[ (lineIndex >> 4) % arraysize(cTab)];
      }

      for (j = 0; j < fr.r.w; j++) {
         line[j] = fr.color;
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * BackMarkDirty --
 *
 *    Mark a region of the backbuffer as dirty. We'll copy it to the
 *    front buffer and ask the host to update it on the next
 *    BackUpdate().
 *
 *-----------------------------------------------------------------------------
 */

static void
BackMarkDirty(Rect rect)  // IN
{
   back.dirtyRects[back.numDirtyRects++] = rect;
}


/*
 *-----------------------------------------------------------------------------
 *
 * BackUpdate --
 *
 *    Copy all dirty regions of the backbuffer to the frontbuffer, and
 *    send updates to the SVGA device. Clears the dirtyRects list.
 *
 *    For flow control, this also waits for the host to process the
 *    batch of updates we just queued into the FIFO.
 *
 *-----------------------------------------------------------------------------
 */

static void
BackUpdate()  // IN
{
   int rectNum;

   for (rectNum = 0; rectNum < back.numDirtyRects; rectNum++) {
      Rect rect = back.dirtyRects[rectNum];
      uint32 i, j;

      for (i = 0; i < rect.h; i++) {
         uint32 offset = (rect.y + i) * MODE_WIDTH + rect.x;
         uint32 *src = &back.buffer[offset];
         uint32 *dest = &((uint32*) gSVGA.fbMem)[offset];

         for (j = 0; j < rect.w; j++) {
            dest[j] = src[j];
         }
      }

      SVGA_Update(rect.x, rect.y, rect.w, rect.h);
   }

   back.numDirtyRects = 0;
   SVGA_SyncToFence(SVGA_InsertFence());
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongDrawString --
 *
 *    Draw a string of digits, using our silly blocky font. The
 *    string's origin is the top-middle.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongDrawString(uint32 x,         // IN
               uint32 y,         // IN
               const char *str,  // IN
               uint32 strLen)    // IN
{
   const int charW = 4;
   const int charH = 5;
   static const uint8 font[] = {

      0xF1,  // **** ...*
      0x91,  // *..* ...*
      0x91,  // *..* ...*
      0x91,  // *..* ...*
      0xF1,  // **** ...*

      0xFF,  // **** ****
      0x11,  // ...* ...*
      0xFF,  // **** ****
      0x81,  // *... ...*
      0xFF,  // **** ****

      0x9F,  // *..* ****
      0x98,  // *..* *...
      0xFF,  // **** ****
      0x11,  // ...* ...*
      0x1F,  // ...* ****

      0xFF,  // **** ****
      0x81,  // *... ...*
      0xF1,  // **** ...*
      0x91,  // *..* ...*
      0xF1,  // **** ...*

      0xFF,  // **** ****
      0x99,  // *..* *..*
      0xFF,  // **** ****
      0x91,  // *..* ...*
      0xF1,  // **** ...*
   };

   x -= (PONG_DIGIT_PIXEL_SIZE * (strLen * (charW + 1) - 1)) / 2;

   while (*str) {
      int digit = *str - '0';
      if (digit >= 0 && digit <= 9) {
         int i, j;

         for (j = 0; j < charH; j++) {
            for (i = 0; i < charW; i++) {
               if ((font[digit / 2 * 5 + j] << i) & (digit & 1 ? 0x08 : 0x80)) {
                  FillRect pixel = {
                     {x + i * PONG_DIGIT_PIXEL_SIZE,
                      y + j * PONG_DIGIT_PIXEL_SIZE,
                      PONG_DIGIT_PIXEL_SIZE,
                      PONG_DIGIT_PIXEL_SIZE},
                     PONG_PLAYFIELD_COLOR,
                  };
                  BackFill(pixel);
               }
            }
         }
      }
      x += PONG_DIGIT_PIXEL_SIZE * (charW + 1);
      str++;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * DecDigit --
 *
 *    Utility for extracting a decimal digit.
 *
 *-----------------------------------------------------------------------------
 */

static char
DecDigit(int i, int div, Bool blank)
{
   if (blank && i < div) {
      return ' ';
   }
   return (i / div) % 10 + '0';
}


/*
 *-----------------------------------------------------------------------------
 *
 * Expand4to24 --
 *
 *    Expand a 4-bit gray value to 24-bit color.
 *
 *-----------------------------------------------------------------------------
 */

static uint32
Expand4to24(uint8 gray)
{
   uint32 x = (gray << 4) | gray;
   x |= x << 8;
   x |= x << 8;
   return x;
}


/*
 *-----------------------------------------------------------------------------
 *
 * DecompressLogo --
 *
 *    Unpack the logo onto our framebuffer. It uses a simple form of
 *    4-bit RLE compression.
 *
 *-----------------------------------------------------------------------------
 */

static void
DecompressLogo()
{
   uint8 x = 0;
   uint8 y = 0;
   uint8 *src = vmwareLogo;
   uint32 *dest = back.buffer + LOGO_X + (LOGO_Y * MODE_WIDTH);
   uint8 shift = 4;
   uint32 color;
   uint8 runCount = 0;
   uint8 runState = 0;

   while (y < LOGO_HEIGHT) {
      if (runCount) {
         // Plot a pixel.
         *(dest++) = color;
         if (++x == LOGO_WIDTH) {
            x = 0;
            y++;
            dest += MODE_WIDTH - LOGO_WIDTH;
         }
         runCount--;

      } else {
         // Load the next compressed nybble
         uint8 nybble = ((*src) >> shift) & 0xF;
         if (shift ^= 4) {
            src++;
         }

         if (runState) {
            // Received run color already. This is the count.
            runCount = nybble + 1;
            runState = 0;

         } else {
            // New color nybble

            color = Expand4to24(nybble);

            if (nybble == 0 || nybble == 0xF) {
               // Run
               runState = 1;
            } else {
               // Not a run
               runCount = 1;
            }
         }
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongDrawPlayfield --
 *
 *    Redraw the playfield for Pong.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongDrawPlayfield()
{
   int i;

   /*
    * Clear the screen
    */
   FillRect background = {
      {0, 0, MODE_WIDTH, MODE_HEIGHT},
      PONG_BG_COLOR,
   };
   BackFill(background);

   /*
    * Draw the dotted dividing line
    */
   for (i = PONG_DOT_SIZE;
        i <= PONG_DIVIDER_HEIGHT - PONG_DOT_SIZE;
        i += PONG_DOT_SIZE * 2) {
      FillRect dot = {
         {(MODE_WIDTH - PONG_DOT_SIZE) / 2, i,
          PONG_DOT_SIZE, PONG_DOT_SIZE},
	 PONG_PLAYFIELD_COLOR,
      };
      if (pong.drawingMode) {
	i = dot.r.h = PONG_DIVIDER_HEIGHT;
	dot.r.y = 0;
      }
      BackFill(dot);
   }

   /*
    * Draw the VMware logo.
    * This is for bug 695355.
    */
   DecompressLogo();

   /*
    * Draw the score counters.
    *
    * sprintf() is big, so we'll format this the old-fashioned way.
    * Right-justify the left score, and left-justify the right score.
    */
   {
      char scoreStr[7] = "       ";
      char *p = scoreStr;

      *(p++) = DecDigit(pong.scores[0], 100, TRUE);
      *(p++) = DecDigit(pong.scores[0], 10, TRUE);
      *(p++) = DecDigit(pong.scores[0], 1, FALSE);
      p++;
      if (pong.scores[1] >= 100) {
         *(p++) = DecDigit(pong.scores[1], 100, TRUE);
      }
      if (pong.scores[1] >= 10) {
         *(p++) = DecDigit(pong.scores[1], 10, TRUE);
      }
      *(p++) = DecDigit(pong.scores[1], 1, FALSE);

      PongDrawString(MODE_WIDTH/2, PONG_DIGIT_PIXEL_SIZE,
                     scoreStr, sizeof scoreStr);
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongDrawScreen --
 *
 *    Top-level redraw function for Pong. This does a lot of unnecessary
 *    drawing to the backbuffer, but we're careful to only send update
 *    rectangles for a few things:
 *
 *      - When the playfield changes, we update the entire screen.
 *      - Each sprite (the paddles and ball) gets two rectangles:
 *          - One for its new position
 *          - One for its old position
 *
 *    None of these rectangles are ever merged.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongDrawScreen()
{
   PongDrawPlayfield();

   if (pong.playfieldDirty) {
      Rect r = {0, 0, MODE_WIDTH, MODE_HEIGHT};
      BackMarkDirty(r);
      pong.playfieldDirty = FALSE;
   }

   /* Draw all sprites at the current positions */
   BackFill(pong.paddles[0]);
   BackMarkDirty(pong.paddles[0].r);
   BackFill(pong.paddles[1]);
   BackMarkDirty(pong.paddles[1].r);
   BackFill(pong.ball);
   BackMarkDirty(pong.ball.r);

   /* Commit this to the front buffer and the host's screen */
   BackUpdate();

   /* Make sure we erase all sprites at the current positions on the next frame */
   BackMarkDirty(pong.paddles[0].r);
   BackMarkDirty(pong.paddles[1].r);
   BackMarkDirty(pong.ball.r);
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongLaunchBall --
 *
 *    Reset the ball position, and give it a random angle.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongLaunchBall()
{
    /* sin() from 0 to PI/2 */
    static const float sineTable[64] = {
        0.000000, 0.024931, 0.049846, 0.074730, 0.099568, 0.124344, 0.149042, 0.173648,
	0.198146, 0.222521, 0.246757, 0.270840, 0.294755, 0.318487, 0.342020, 0.365341,
	0.388435, 0.411287, 0.433884, 0.456211, 0.478254, 0.500000, 0.521435, 0.542546,
	0.563320, 0.583744, 0.603804, 0.623490, 0.642788, 0.661686, 0.680173, 0.698237,
	0.715867, 0.733052, 0.749781, 0.766044, 0.781831, 0.797133, 0.811938, 0.826239,
	0.840026, 0.853291, 0.866025, 0.878222, 0.889872, 0.900969, 0.911506, 0.921476,
	0.930874, 0.939693, 0.947927, 0.955573, 0.962624, 0.969077, 0.974928, 0.980172,
	0.984808, 0.988831, 0.992239, 0.995031, 0.997204, 0.998757, 0.999689, 1.000000,
    };
    int t;
    float sinT, cosT;

    pong.ballPos.x = MODE_WIDTH / 2;
    pong.ballPos.y = MODE_HEIGHT / 2;

    /* Limit the random angle to avoid those within 45 degrees of vertical */
    t = 32 + (Random32() & 31);

    sinT = sineTable[t];
    cosT = -sineTable[(t + 32) & 63];

    sinT *= pong.ballSpeed;
    cosT *= pong.ballSpeed;

    switch (Random32() & 3) {
    case 0:
	pong.ballVelocity.x = sinT;
	pong.ballVelocity.y = cosT;
	break;
    case 1:
	pong.ballVelocity.x = -sinT;
	pong.ballVelocity.y = cosT;
	break;
    case 2:
	pong.ballVelocity.x = -sinT;
	pong.ballVelocity.y = -cosT;
	break;
    case 3:
	pong.ballVelocity.x = sinT;
	pong.ballVelocity.y = -cosT;
	break;
    }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongInit --
 *
 *    Initialize all game variables, including sprite location/size/color.
 *    Requires that SVGA has already been initialized.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongInit()
{
   pong.scores[0] = 0;
   pong.scores[1] = 0;
   pong.playfieldDirty = TRUE;

   pong.paddlePos[0] = pong.paddlePos[1] = MODE_HEIGHT / 2;

   pong.paddles[0].r.x = 10;
   pong.paddles[0].r.w = 16;
   pong.paddles[0].r.h = 64;
   pong.paddles[0].color = PONG_SPRITE_COLOR;

   pong.paddles[1].r.x = MODE_WIDTH - 16 - 10;
   pong.paddles[1].r.w = 16;
   pong.paddles[1].r.h = 64;
   pong.paddles[1].color = PONG_SPRITE_COLOR;

   pong.ball.r.w = 16;
   pong.ball.r.h = 16;
   pong.ball.color = PONG_SPRITE_COLOR;

   pong.ballSpeed = 400;
   PongLaunchBall();
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongUpdateMotion --
 *
 *    Perform motion updates for the ball and paddles. This includes
 *    bounce/goal detection.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongUpdateMotion(float dt)  // IN
{
   int playableWidth = MODE_WIDTH - pong.ball.r.w;
   int playableHeight = MODE_HEIGHT - pong.ball.r.h;
   int i;

   pong.ballPos.x += pong.ballVelocity.x * dt;
   pong.ballPos.y += pong.ballVelocity.y * dt;

   for (i = 0; i < 2; i++) {
      int pos = pong.paddlePos[i] + pong.paddleVelocities[i] * dt;
      pong.paddlePos[i] = MIN(MODE_HEIGHT - pong.paddles[i].r.h, MAX(0, pos));
      pong.paddles[i].r.y = (int)pong.paddlePos[i];
   }

   if (pong.ballPos.x >= playableWidth) {
      /* Goal off the right edge */
      pong.scores[0]++;
      pong.playfieldDirty = TRUE;
      PongLaunchBall();
   }

   if (pong.ballPos.x <= 0) {
      /* Goal off the left edge */
      pong.scores[1]++;
      pong.playfieldDirty = TRUE;
      PongLaunchBall();
   }

   if (pong.ballPos.y >= playableHeight) {
      /* Bounce off the bottom edge */
      pong.ballVelocity.y = -pong.ballVelocity.y;
      pong.ballPos.y = playableHeight - (pong.ballPos.y - playableHeight);
   }

   if (pong.ballPos.y <= 0) {
      /* Bounce off the top edge */
      pong.ballVelocity.y = -pong.ballVelocity.y;
      pong.ballPos.y = -pong.ballPos.y;
   }

   pong.ballPos.y = MIN(playableHeight, pong.ballPos.y);
   pong.ballPos.y = MAX(0, pong.ballPos.y);

   pong.ball.r.x = (int)pong.ballPos.x;
   pong.ball.r.y = (int)pong.ballPos.y;

   /*
    * Lame collision detection between ball and paddles. Really we
    * should be testing the ball's entire path over this time step,
    * not just the ball's new position. Using the current
    * implementation, it's possible for the ball to move through a
    * paddle if it's going fast enough or our frame rate is slow
    * enough.
    */
   for (i = 0; i < 2; i++) {
      /*
       * Only bounce off the paddle when we're moving toward it, to
       * prevent the ball from getting stuck inside the paddle
       */
      if ((pong.paddles[i].r.x > MODE_WIDTH / 2) == (pong.ballVelocity.x > 0) &&
          RectTestIntersection(&pong.ball.r, &pong.paddles[i].r)) {
         /*
          * Boing! The ball bounces back, plus it gets a little spin
          * if the paddle itself was moving at the time.
          */
         pong.ballVelocity.x = -pong.ballVelocity.x;
         pong.ballVelocity.y += pong.paddleVelocities[i];
         pong.ballVelocity.y = MIN(pong.ballVelocity.y, pong.ballSpeed * 2);
         pong.ballVelocity.y = MAX(pong.ballVelocity.y, -pong.ballSpeed * 2);
      }
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongKeyboardPlayer --
 *
 *    A human player, using the up and down arrows on a keyboard.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongKeyboardPlayer(int playerNum,   // IN
                   float maxSpeed,  // IN
                   float accel)     // IN
{
   float v = pong.paddleVelocities[playerNum];
   Bool up = Keyboard_IsKeyPressed(0x48);
   Bool down = Keyboard_IsKeyPressed(0x50);

   if (up && !down) {
      v -= accel;
   } else if (down && !up) {
      v += accel;
   } else {
      v = 0;
   }

   v = MIN(maxSpeed, MAX(-maxSpeed, v));
   pong.paddleVelocities[playerNum] = v;
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongAbsMousePlayer --
 *
 *    A human player, controlled with the Y axis of the absolute mouse.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongAbsMousePlayer(int playerNum)   // IN
{
   int currentY = pong.paddles[playerNum].r.y;
   int newY = currentY;
   VMMousePacket p;
   Bool mouseMoved = FALSE;

   while (VMBackdoor_MouseGetPacket(&p)) {
      newY = (p.y * MODE_HEIGHT / 0xFFFF) - pong.paddles[playerNum].r.h / 2;
      newY = MAX(0, newY);
      newY = MIN(MODE_HEIGHT - pong.paddles[playerNum].r.h, newY);
      mouseMoved = TRUE;
   }

   if (newY != currentY && mouseMoved) {
      pong.paddleVelocities[playerNum] = (newY - currentY) * (float)PONG_FRAME_RATE;
   }
}


/*
 *-----------------------------------------------------------------------------
 *
 * PongComputerPlayer --
 *
 *    Simple computer player. Always moves its paddle toward the ball.
 *
 *-----------------------------------------------------------------------------
 */

static void
PongComputerPlayer(int playerNum,   // IN
                   float maxSpeed)  // IN
{
   int paddleCenter = pong.paddles[playerNum].r.y + pong.paddles[playerNum].r.h / 2;
   int ballCenter = pong.ball.r.y + pong.ball.r.h / 2;
   int distance = ballCenter - paddleCenter;

   pong.paddleVelocities[playerNum] = distance * maxSpeed * (1.0f / MODE_HEIGHT);
}


/*
 *-----------------------------------------------------------------------------
 *
 * IrqHandler --
 *
 *    IRQ handler for drawing mode auto-detect on startup.
 *
 *-----------------------------------------------------------------------------
 */

fastcall void
IrqHandler(KeyEvent *event)
{
  static const uint8 states[] = { 0x19, 0x13, 0x17, 0x20, 0x12 };
  static int matched = 0;

  if (!event->pressed) {
    if (event->rawKey == states[matched]) {
      matched++;
    } else {
      matched = 0;
    }
  }

  if (matched == arraysize(states)) {
    matched = 0;
    pong.drawingMode ^= TRUE;
    pong.playfieldDirty = TRUE;
  }
}


/*
 *-----------------------------------------------------------------------------
 *
 * main --
 *
 *    Initialization and main loop.
 *
 *-----------------------------------------------------------------------------
 */

void
main(void)
{
   Intr_Init();
   SVGA_Init();
   SVGA_SetMode(MODE_WIDTH, MODE_HEIGHT, 32);
   back.buffer = (uint32*) (gSVGA.fbMem + MODE_WIDTH * MODE_HEIGHT * sizeof(uint32));

   Keyboard_Init();
   Keyboard_SetHandler(IrqHandler);
   VMBackdoor_MouseInit(TRUE);
   PongInit();

   Timer_InitPIT(PIT_HZ / PONG_FRAME_RATE);
   Intr_SetMask(0, TRUE);

   while (1) {
      PongKeyboardPlayer(0, 1000, 50);
      PongAbsMousePlayer(0);
      PongComputerPlayer(1, 2000);

      PongUpdateMotion(1.0 / PONG_FRAME_RATE);
      PongDrawScreen();

      Intr_Halt();
   }
}
