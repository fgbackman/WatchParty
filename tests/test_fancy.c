/* Tests for the fancy-words filter — build: cc -I../src test_fancy.c ../src/wp_fancy.c */
#include "../src/wp_fancy.h"
#include <stdio.h>
#include <string.h>

static int fails = 0;
static void expect(const char *in, const char *want)
{
    char out[512];
    wp_fancy(in, out, sizeof out);
    if (strcmp(out, want) != 0) { fails++; printf("FAIL: \"%s\"\n   got  \"%s\"\n   want \"%s\"\n", in, out, want); }
}

int main(void)
{
    /* forms */
    expect("what the fuck", "what the flabbergast");
    expect("this is fucking great", "this is flabbergasting great");
    expect("I fucked up", "I flabbergasted up");
    expect("that movie was shitty", "that movie was sesquipedalian");
    expect("you asshole", "you antiestablishmentarian");
    expect("stop bitching", "stop bloviating");
    expect("damn it", "disenfranchise it");
    expect("go to hell", "go to hippopotomonstrosesquippedaliophobia");
    expect("total bullshit", "total bamboozlement");
    expect("ate fr no cap af", "ate fr no cap as flabbergast");
    /* case */
    expect("FUCK", "FLABBERGAST");
    expect("Shit happens", "Sesquipedalianism happens");
    expect("WTF", "WHAT THE FLABBERGAST");
    expect("Wtf", "What the flabbergast");
    /* disguises */
    expect("fuuuuuck", "flabbergast");
    expect("sh1t", "sesquipedalianism");
    expect("a$$", "antidisestablishmentarianism");
    expect("b!tch", "blatherskite");
    expect("f*ck", "flabbergast");
    expect("f**k this", "flabbergast this");
    expect("absofuckinglutely", "absoflabbergastinglutely");
    expect("unfuckingbelievable", "unflabbergastingbelievable");
    /* punctuation stays */
    expect("fuck!!", "flabbergast!!");
    expect("#shit", "#sesquipedalianism");
    expect("(damn)", "(disenfranchise)");
    expect("mother-fucker", "mother-flibbertigibbet");
    expect("fuckin' hell", "flabbergastin'' hippopotomonstrosesquippedaliophobia");
    /* slurs become compliments */
    expect("you dyke", "you dynamo");
    expect("retarded idea", "resplendent idea");
    /* innocent words survive */
    expect("pass the class assignment", "pass the class assignment");
    expect("as I said", "as I said");
    expect("shiitake mushrooms", "shiitake mushrooms");
    expect("Scunthorpe", "Scunthorpe");
    expect("hello there", "hello there");
    expect("cocktail", "cocktail");
    expect("1000 views, $5, ***", "1000 views, $5, ***");
    expect("https://www.youtube.com/watch?v=fuckshit123", "https://www.youtube.com/watch?v=fuckshit123");
    expect("niggardly", "niggardly");
    expect("😂🔥 lol", "😂🔥 lol");
    /* truncation is safe */
    {
        char tiny[8];
        wp_fancy("fuck", tiny, sizeof tiny);
        if (strlen(tiny) != 7) { fails++; printf("FAIL: truncation gave \"%s\"\n", tiny); }
    }

    if (fails) { printf("test_fancy: %d failure(s)\n", fails); return 1; }
    printf("test_fancy: all passed\n");
    return 0;
}
