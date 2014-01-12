#include <PS2Keyboard.h>
#include <SdFat.h>
#include <math.h>
#include "c64.h"
#include "aes256.h"

#define PIN_RED 16
#define PIN_BLUE 17
#define PIN_GREEN 18
#define PIN_VBLANK 8
#define PIN_HBLANK 7
#define RGB_OUT GPIOB_PDOR
// PortB[0:3, 16:19] = {16, 17, 19, 18, 0, 1, 32, 25}

#define PIN_PS2_DATA 2
#define PIN_PS2_CLOCK 3

#define PIN_SPI_CHIPSEL 25

volatile uint8_t aes_key[32];
aes256_context aes_context;

#define CSTATE_EDITOR 0
#define CSTATE_COMMAND 1
#define CSTATE_COMMAND_OUTPUT 2

volatile int currentLine = 0;
volatile int currentFrame = 0;
int commandState = 0;

PS2Keyboard keys;
Sd2Card card;

SdVolume volume;
SdFile root;

#define UPPER_BORDER 40
#define COLS 40
#define ROWS 40
volatile unsigned char borderColor = 0x0;

IntervalTimer timer0;

char displayBuffer[COLS * ROWS];
unsigned char charaddrs[COLS];
unsigned char invertedChar[8]; // for cursor

char memory[4096];
unsigned int memoryUsed = 0;

int currentKey = 0;
unsigned char cursorX = 0;
unsigned char cursorY = 0;
unsigned int cursor = 0;

unsigned int editorBufferSize = 0;
unsigned int editorScrollLine = 0;

char currentFileName[] = "scratch0.txt\0";

char* displayAt(int x, int y) {
  if (x<0) x=0;
  if (y<0) y=0;
  if (x>=COLS) x=COLS-1;
  if (y>=ROWS) y=ROWS-1;
  return displayBuffer + x+y*COLS;
}

void putPixel(int x,int y,char color) {
  if (x>=0 && y>=0 && x<COLS && y<ROWS) {
    *displayAt(x,y) = color;
  }
}

void fillScreen(char c) {
  for (int y=0; y<ROWS; y++) {
    for (int x=0; x<COLS; x++) {
      displayBuffer[y*COLS+x] = c;
    }
  }
}

void vgaLine() {
  if (currentLine>1) {
    digitalWrite(PIN_VBLANK, 1);
  } else {
    digitalWrite(PIN_VBLANK, 0);
  }

  digitalWrite(PIN_HBLANK, 0);
  RGB_OUT = 0x00;
  RGB_OUT = 0x00;
  RGB_OUT = 0x00;
  RGB_OUT = 0x00;
  digitalWrite(PIN_HBLANK, 1);

  currentLine++;
  currentLine = currentLine%525;
  
  RGB_OUT = 0x0;

  if (currentLine<UPPER_BORDER) {
    RGB_OUT = borderColor;

    if (currentLine<8) {
      // cursor
      invertedChar[currentLine] = FONT[((*(displayBuffer + COLS*cursorY + cursorX))<<3) + currentLine] ^ 0xff;
    }
  }
  else if (currentLine>=(8*ROWS+UPPER_BORDER)) {
    RGB_OUT = borderColor;

  } else {
    RGB_OUT = borderColor;

    unsigned char lineY = (currentLine-UPPER_BORDER) >> 3;
    unsigned char fontLine = ((currentLine-UPPER_BORDER)%8);
    
    char* message = displayBuffer + COLS*lineY;

    unsigned char* fontP = FONT + fontLine;

    for (int x=COLS-1; x>=0; x--) {
      charaddrs[x] = fontP[message[x] << 3];
    }

    if (cursorY==lineY) {
      charaddrs[cursorX] = invertedChar[fontLine];
      for (int i=0; i<32; i++) __asm("nop\n");
    } else {
      for (int i=0; i<38; i++) __asm("nop\n");
    }

    for (register unsigned char x=0; x<40; x++) {
      register unsigned char c = charaddrs[x];

      RGB_OUT = c >> 7;
      RGB_OUT = c >> 6;
      RGB_OUT = c >> 5;
      RGB_OUT = c >> 4;
      RGB_OUT = c >> 3;
      RGB_OUT = c >> 2;
      RGB_OUT = c >> 1;
      RGB_OUT = c;
    }


    RGB_OUT = borderColor;
  }
}

void drawHorizLine(int y, int c) {
  for (int x=0; x<COLS; x++) {
    displayBuffer[y*COLS+x] = c;
  }
}

void clearScreen() { 
  fillScreen(' ');
}

void listDirectory(SdFile* dir) {
  SdFile entry;
  while (entry.openNext(dir, O_READ)) {
    cursorY++;
  
    entry.getFilename(displayAt(cursorX,cursorY));
    if (entry.isDir()) {
      sprintf(displayAt(cursorX+13,cursorY), "(DIR)");
    } else {
      sprintf(displayAt(cursorX+13,cursorY), "%d", entry.fileSize());
    }

    entry.close();
  }
  cursorY++;
}

void returnToEditor() {
  commandState = CSTATE_EDITOR;
  renderEditorBuffer();
}

void listStorage() {
  pl("Root:");
  root.close();
  root.openRoot(&volume);
  listDirectory(&root);
}

int loadBuffer(char* fileName) {
  int res = 0;
  SdFile file;
  if (file.open(&root, fileName, O_READ)) {
    editorBufferSize = file.read((uint8_t*)memory, 2048);
    cursor = 0;
    file.close();
    return editorBufferSize;
  }
  return res;
}

int saveBuffer(char* fileName, int size) {
  int res = -1;
  SdFile file;
  if (file.open(&root, fileName, O_WRITE | O_CREAT | O_TRUNC)) {
    res = file.write((const uint8_t*)memory, size);
    file.close();
  } else {
    pl("Couldn't write. card locked?");
  }
  return res;
}

int readCommandArg(char* start, char* arg, int maxlen) {
  int idx = 0;
  while (idx<maxlen) {
    char c = *start;
    if (c == '\n' || c == 0 || c==' ') break;
    else {
      *(arg+idx) = c;
      idx++;
      start++;
    }
  }
  return idx;
}

void clearBuffer() {
  editorBufferSize=1;
  memory[0]=' ';
  cursor=0;
}

const int keylen = 32;
    
void interpretCommand(char* command) {
  if (strncmp(command,"ls\n",3) == 0) {
    clearScreen();
    listStorage();
  }
  else if (strncmp(command,"clear",5) == 0) {
    clearBuffer();
    returnToEditor();
  } else if (strncmp(command,"save",4) == 0) {
    // write buffer to file

    int len = readCommandArg(command+5, currentFileName, 8);
  
    // sd file
  
    if (len>0) {
      currentFileName[len] = '.';
      currentFileName[len+1] = 't';
      currentFileName[len+2] = 'x';
      currentFileName[len+3] = 't';
      currentFileName[len+4] = 0;
    }

    clearScreen();

    pl("Write:");
    pl(currentFileName);

    int res = saveBuffer(currentFileName, editorBufferSize);
    sprintf(displayAt(cursorX,cursorY++),"%d bytes written.",res);
  } else if (strncmp(command,"load",4) == 0) {
    // read buffer from file

    int len = readCommandArg(command+5, currentFileName, 8);
    if (len>0) {
      currentFileName[len] = '.';
      currentFileName[len+1] = 't';
      currentFileName[len+2] = 'x';
      currentFileName[len+3] = 't';
      currentFileName[len+4] = 0;
    }

    int res = loadBuffer(currentFileName);
    if (!res) pl("file not found.");
    returnToEditor();
    
  } else if (strncmp(command,"rnd",3) == 0) {
    clearBuffer();
    for (int i=0; i<keylen; i++) {
      memory[i] = random(256);
    }
    editorBufferSize = keylen;
    returnToEditor();
  } else if (strncmp(command,"tune",4) == 0) {
    // copy 32 buffer bytes to key memory
    
    for (int i=0; i<keylen; i++) {
      aes_key[i] = memory[i];
    }
    aes256_init(&aes_context, (uint8_t*)aes_key);
    
    pl("aes256 key loaded from buffer.");
    pl("press [tab] to return.");
  } else if (strncmp(command,"get",3) == 0) {
    // poll server for new crypted messages
    
    Serial.write((const uint8_t*)command,strlen(command));
    
    delay(500); // give server some time
    
    clearBuffer();
    editorBufferSize = 0;
    while (Serial.available() && editorBufferSize<4096) {
      memory[editorBufferSize++] = Serial.read();
    }
    
    returnToEditor();
  } else if (strncmp(command,"put",3) == 0) {
    // poll server for new crypted messages
    
    sprintf(&command[4], "%s ", currentFileName);
    sprintf(&command[4+8], " %d ", editorBufferSize-1);
    
    Serial.write((const uint8_t*)command,strlen(command));
    Serial.write((const uint8_t*)memory, editorBufferSize);
    
    pl(command);
    pl("buffer sent to server.");
  }
  cursorY++;
  
  if (cursorY>=ROWS-1) {
    cursorY=1;
    cursorX=0;
  }
}

void insertLine() {
  for (int y=ROWS-2; y>cursorY; y--) {
    for (int x=0; x<COLS; x++) {
      displayBuffer[y*COLS + x] = displayBuffer[(y-1)*COLS + x];
    }
  }
  drawHorizLine(cursorY, ' ');
}

char lastKey = 0;

void renderEditorBuffer() {
  bool done = false;
  int bufferIdx = 0;
  int displayIdx = 0;
  int cursorDispIdx = 0;

  clearScreen();

  if (cursor<0) cursor = 0;
  if (cursor>=editorBufferSize && editorBufferSize>0) cursor = editorBufferSize-1;

  while (!done) {
    char bufc = memory[bufferIdx];

    displayBuffer[displayIdx] = bufc;

    if (cursor == bufferIdx) {
      cursorDispIdx = displayIdx;
    }

    if (bufc=='\n') {
      displayIdx+=COLS-(displayIdx%COLS);
    } else {
      displayIdx++;
    }

    bufferIdx++;
    if (bufferIdx>=editorBufferSize || displayIdx>=COLS*ROWS) done = true;
  }

  if (commandState == CSTATE_EDITOR) {
    cursorX = cursorDispIdx%COLS;
    cursorY = cursorDispIdx/COLS;

    sprintf(displayAt(0,ROWS-1),"%s %d/%d",currentFileName, cursor, editorBufferSize);
  } else if (commandState == CSTATE_COMMAND) {

    drawHorizLine(ROWS-1,' ');
    *displayAt(0,ROWS-1) = '>';
  }
}


void editorKeyInput() {
  int k = keys.read();

  if (k>=0) {
    lastKey = k;

    if (commandState == CSTATE_EDITOR) {
      if (k==PS2_LEFTARROW) {
        if (cursor>0) cursor--;
      } else if (k==PS2_RIGHTARROW) {
        if (cursor<editorBufferSize) {
          cursor++;
        }
      } else if (k==PS2_DOWNARROW) {
        // look for next newline
        if (cursor<editorBufferSize) {
          while (cursor < editorBufferSize-1) {
            if (memory[++cursor]=='\n') {
              cursor++;
              break;
            }
          }
        }
      } else if (k==PS2_UPARROW) {
        // look for previous-1 newline
        if (cursor>0) {
          while (cursor > 0) {
            if (memory[--cursor]=='\n') {
              break;
            }
          }
        }
      } else if (k==PS2_TAB) {
        commandState = CSTATE_COMMAND;
        cursorX = 0;
        cursorY = ROWS-1;
      } else if (k==PS2_DELETE) {
        // remove char
        if (editorBufferSize>0) {
          editorBufferSize--;
          for (int i=cursor; i<editorBufferSize; i++) {
            memory[i] = memory[i+1];
          }
        }
      } 
      else if (k==PS2_F1) {
        // encrypt the buffer (16 byte blocks)
        for (int i=0; i<(40*40)/16; i+=16) {
          aes256_encrypt_ecb(&aes_context, (uint8_t*)&memory[i]);
        }
        
      }
      else if (k==PS2_F2) {
        // decrypt the buffer
        for (int i=0; i<(40*40)/16; i+=16) {
          aes256_decrypt_ecb(&aes_context, (uint8_t*)&memory[i]);
        }
      }
      else if (k==PS2_F5) {
        // write to serial
        Serial.write((const uint8_t*)memory, editorBufferSize);

      }
      else if (k==PS2_F6) {
        // read from serial
        clearBuffer();
        editorBufferSize = 0;
        while (Serial.available() && editorBufferSize<4096) {
          memory[editorBufferSize++] = Serial.read();
        }
      }
      else {
        // insert char
        if (k==13) k=10; // return -> newline
        editorBufferSize++;
        for (int i=editorBufferSize-1; i>=cursor+1; i--) {
          memory[i] = memory[i-1];
        }
        memory[cursor++] = k;
      }

      renderEditorBuffer();

    } else if (commandState == CSTATE_COMMAND) {
      // command line
      if (k==PS2_TAB) {
        commandState = CSTATE_EDITOR;

        renderEditorBuffer();
      } else if (k==PS2_ENTER) {
        commandState = CSTATE_COMMAND_OUTPUT;
        *displayAt(cursorX,cursorY) = '\n';
        *displayAt(cursorX+1,cursorY) = 0;
        cursorX=0;
        cursorY=0;
        interpretCommand(displayAt(0,ROWS-1));
      } else if (k==PS2_DELETE) {
        *displayAt(cursorX--,cursorY) = ' ';
      } else {
        *displayAt(cursorX++,cursorY) = k;
      }
    } else if (commandState == CSTATE_COMMAND_OUTPUT) {
      if (k==PS2_TAB || k==PS2_ENTER) {
        commandState = CSTATE_EDITOR;

        renderEditorBuffer();
      } else if (k==PS2_ENTER) {
        // todo: append output to buffer
      }
    }
  }
}


void pl(char* line) {
  sprintf(displayAt(0,cursorY++), line);
}

// appendLine to editor buffer
void al(char* line) {
  int len = sprintf(&memory[editorBufferSize++], line);
  editorBufferSize += len-1;
}

void initStorage() {

  pinMode(PIN_SPI_CHIPSEL, OUTPUT);
  if (!card.init(SPI_HALF_SPEED, PIN_SPI_CHIPSEL)) {
    al("Failed to init SD.\n");
  } else {
    
    switch(card.type()) {
      case SD_CARD_TYPE_SD1:
        al("SD1 CARD PRESENT.\n"); break;
      case SD_CARD_TYPE_SD2:
        al("SD2 CARD PRESENT.\n"); break;
      case SD_CARD_TYPE_SDHC:
        al("SDHC CARD PRESENT.\n"); break;
      default:
        al("MYSTERY CARD PRESENT.\n"); break;
    }
    
    if (!volume.init(&card)) {
      al("NO FAT VOLUME ON CARD.\n");
    } else {
      
      uint32_t volumesize;
      volumesize = volume.blocksPerCluster();    // clusters are collections of blocks
      volumesize *= volume.clusterCount();       // we'll have a lot of clusters
      volumesize *= 512;
      
      int len = sprintf(&memory[editorBufferSize],"Volume Size: %d KB\n",volumesize/1024);
      editorBufferSize+=len;
      
      if (!root.openRoot(&volume)) {
        al("Error opening volume root.\n");
      }
    }
  }
}

void printFreeMemory() {
  int len = sprintf(&memory[editorBufferSize],"Free Memory: %d bytes\n",sizeof(memory)-memoryUsed);
  editorBufferSize += len;
}

void setup()   {
  Serial.begin(true); // USB is always 12 Mbit/sec
  delay(500);
  
  pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT);
  pinMode(PIN_GREEN, OUTPUT);
  pinMode(PIN_VBLANK, OUTPUT);
  pinMode(PIN_HBLANK, OUTPUT);
  
  timer0.begin(vgaLine, 31.5);
  
  keys.begin(PIN_PS2_DATA, PIN_PS2_CLOCK);

  initStorage();
  printFreeMemory();
  
  aes256_init(&aes_context, (uint8_t*)aes_key);
  al("AES 256 crypto initialized.\n");

  al("RS-16 is at your command.\n");

  renderEditorBuffer();

  //disableInterrupts(); // optional to get a less fuzzy picture
}

uint32_t tick = 0;
void loop()                     
{
  tick++;
  if (tick>1000) {
    editorKeyInput();
    tick = 0;
  }
}


void disableInterrupts() {
  NVIC_DISABLE_IRQ(IRQ_WDOG);
  NVIC_DISABLE_IRQ(IRQ_I2C0);
  NVIC_DISABLE_IRQ(IRQ_I2S0_TX);
  NVIC_DISABLE_IRQ(IRQ_I2S0_RX);
  NVIC_DISABLE_IRQ(IRQ_SPI0);
  NVIC_DISABLE_IRQ(IRQ_LLWU);
  NVIC_DISABLE_IRQ(IRQ_DMA_CH0);
  NVIC_DISABLE_IRQ(IRQ_DMA_CH1);
  NVIC_DISABLE_IRQ(IRQ_DMA_CH2);
  NVIC_DISABLE_IRQ(IRQ_DMA_CH3);
  NVIC_DISABLE_IRQ(IRQ_DMA_ERROR);
  NVIC_DISABLE_IRQ(IRQ_FTFL_COMPLETE);
  NVIC_DISABLE_IRQ(IRQ_FTFL_COLLISION);
  NVIC_DISABLE_IRQ(IRQ_LOW_VOLTAGE);
  NVIC_DISABLE_IRQ(IRQ_ADC0);
  NVIC_DISABLE_IRQ(IRQ_CMP0);
  NVIC_DISABLE_IRQ(IRQ_CMP1);
  NVIC_DISABLE_IRQ(IRQ_FTM0);
  NVIC_DISABLE_IRQ(IRQ_FTM1);
  NVIC_DISABLE_IRQ(IRQ_CMT);
  NVIC_DISABLE_IRQ(IRQ_RTC_SECOND);
  NVIC_DISABLE_IRQ(IRQ_RTC_ALARM);
  /*NVIC_DISABLE_IRQ(IRQ_PIT_CH0);
  NVIC_DISABLE_IRQ(IRQ_PIT_CH1);
  NVIC_DISABLE_IRQ(IRQ_PIT_CH2);
  NVIC_DISABLE_IRQ(IRQ_PIT_CH3);*/
  NVIC_DISABLE_IRQ(IRQ_PDB);
  NVIC_DISABLE_IRQ(IRQ_TSI);
  NVIC_DISABLE_IRQ(IRQ_MCG);
  NVIC_DISABLE_IRQ(IRQ_LPTMR);
  NVIC_DISABLE_IRQ(IRQ_SOFTWARE);
  /*NVIC_DISABLE_IRQ(IRQ_PORTA);
  NVIC_DISABLE_IRQ(IRQ_PORTB);
  NVIC_DISABLE_IRQ(IRQ_PORTC);
  NVIC_DISABLE_IRQ(IRQ_PORTD);
  NVIC_DISABLE_IRQ(IRQ_PORTE);*/
  NVIC_DISABLE_IRQ(IRQ_USBOTG); // this is the flash uploader
  NVIC_DISABLE_IRQ(IRQ_USBDCD); 
  NVIC_DISABLE_IRQ(IRQ_UART0_LON);
  NVIC_DISABLE_IRQ(IRQ_UART0_STATUS);
  NVIC_DISABLE_IRQ(IRQ_UART0_ERROR);
  NVIC_DISABLE_IRQ(IRQ_UART1_STATUS);
  NVIC_DISABLE_IRQ(IRQ_UART1_ERROR);
  NVIC_DISABLE_IRQ(IRQ_UART2_STATUS);
  NVIC_DISABLE_IRQ(IRQ_UART2_ERROR);
}
