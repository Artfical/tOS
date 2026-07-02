/* demo_song_embed.s - links demo_song.wav binary into kernel rodata */
    .section .rodata
    .global demo_song_pcm
    .global demo_song_pcm_end
demo_song_pcm:
    /* skip 44-byte WAV header, embed raw PCM only */
    .incbin "kernel/audio/demo_song.wav", 44
demo_song_pcm_end:
