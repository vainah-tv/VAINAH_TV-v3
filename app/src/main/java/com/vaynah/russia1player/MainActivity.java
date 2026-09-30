package com.vaynah.russia1player;

import android.app.Activity;
import android.app.AlertDialog;
import android.content.Intent;
import android.graphics.Color;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.text.Html;
import android.view.GestureDetector;
import android.view.Gravity;
import android.view.MotionEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.webkit.WebSettings;
import android.webkit.WebView;
import android.webkit.WebViewClient;
import android.widget.Button;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.SeekBar;
import android.widget.TextView;

import androidx.media3.common.MediaItem;
import androidx.media3.common.MimeTypes;
import androidx.media3.common.PlaybackException;
import androidx.media3.common.Player;
import androidx.media3.exoplayer.ExoPlayer;
import androidx.media3.ui.PlayerView;

import java.io.ByteArrayOutputStream;
import java.io.InputStream;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;

public class MainActivity extends Activity {
    private static final String NEWS_URL = "https://gtrkvainah.ru/content/";
    private static final String SITE_URL = "https://gtrkvainah.ru/";
    private static final String[] NAMES = {"РОССИЯ 1HD", "РОССИЯ 24", "ВАЙНАХ ТВ", "РАДИО ВАЙНАХ"};
    private static final String[] STREAMS = {
            "https://live.smotrim.ru/vgtrk/0/russia1-hd/1080p.m3u8",
            "https://live-gtrk.smotrim.ru/vgtrk/grozniy/russia24-sd/track_103_35f01932/chunklist.m3u8",
            "https://live-gtrk.smotrim.ru/vgtrk/grozniy/russia1-sd/track_103_947c7bd7/chunklist.m3u8",
            "https://podcast-gtrk.smotrim.ru/vgtrk/grozniy/radio_russia/track_1001_85855c03/chunklist.m3u8"
    };

    private ExoPlayer player;
    private PlayerView playerView;
    private ImageView radioArt;
    private LinearLayout chrome;
    private FrameLayout playerArea;
    private WebView newsView;
    private TextView status;
    private Button muteButton;
    private int channel;
    private float volume = 1f;
    private boolean muted;
    private boolean fullscreen;
    private boolean showingPlayer;

    @Override public void onCreate(Bundle state) {
        super.onCreate(state);
        getWindow().setStatusBarColor(Color.BLACK);
        getWindow().setNavigationBarColor(Color.BLACK);
        if (state != null) {
            channel = state.getInt("channel", 0);
            volume = state.getFloat("volume", 1f);
            muted = state.getBoolean("muted", false);
            fullscreen = state.getBoolean("fullscreen", false);
            showingPlayer = state.getBoolean("showingPlayer", false);
        }

        player = new ExoPlayer.Builder(this).build();
        player.setAudioAttributes(new androidx.media3.common.AudioAttributes.Builder()
                .setUsage(androidx.media3.common.C.USAGE_MEDIA)
                .setContentType(androidx.media3.common.C.AUDIO_CONTENT_TYPE_MOVIE).build(), true);
        player.addListener(new Player.Listener() {
            @Override public void onPlaybackStateChanged(int state) {
                if (status == null) return;
                if (state == Player.STATE_BUFFERING) status.setText("Подключение к трансляции…");
                else if (state == Player.STATE_READY) status.setText(NAMES[channel] + " · прямой эфир");
                else if (state == Player.STATE_ENDED) status.setText("Трансляция завершилась. Выберите канал снова.");
            }
            @Override public void onPlayerError(PlaybackException error) {
                if (status != null) status.setText("Не удалось открыть поток. Проверьте сеть или повторите выбор канала.");
            }
        });

        makeUi();
        selectChannel(channel);
        showPlayer(showingPlayer);
        setFullscreen(fullscreen && showingPlayer);
    }

    private int dp(int value) { return (int) (value * getResources().getDisplayMetrics().density + .5f); }

    private Button button(String label) {
        Button b = new Button(this);
        b.setText(label);
        b.setAllCaps(false);
        b.setTextColor(Color.WHITE);
        b.setTextSize(15);
        b.setBackgroundTintList(android.content.res.ColorStateList.valueOf(0xff26344b));
        return b;
    }

    private void makeUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setBackgroundColor(0xff101723);
        setContentView(root);
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            if (!fullscreen) {
                if (Build.VERSION.SDK_INT >= 30) {
                    android.graphics.Insets i = insets.getInsets(WindowInsets.Type.systemBars());
                    v.setPadding(i.left, i.top, i.right, i.bottom);
                } else {
                    v.setPadding(insets.getSystemWindowInsetLeft(), insets.getSystemWindowInsetTop(),
                            insets.getSystemWindowInsetRight(), insets.getSystemWindowInsetBottom());
                }
            } else v.setPadding(0, 0, 0, 0);
            return insets;
        });

        LinearLayout top = new LinearLayout(this);
        top.setGravity(Gravity.CENTER_VERTICAL);
        top.setPadding(dp(10), dp(8), dp(10), dp(8));
        root.addView(top, new LinearLayout.LayoutParams(-1, dp(64)));

        TextView title = new TextView(this);
        title.setText("ГТРК ВАЙНАХ");
        title.setTextColor(Color.WHITE);
        title.setTextSize(20);
        title.setGravity(Gravity.CENTER_VERTICAL);
        title.setTypeface(null, android.graphics.Typeface.BOLD);
        top.addView(title, new LinearLayout.LayoutParams(0, -1, 1));

        Button live = button("ЭФИР");
        top.addView(live, new LinearLayout.LayoutParams(dp(92), dp(48)));
        live.setOnClickListener(v -> showPlayer(true));

        Button site = button("НАШ САЙТ");
        LinearLayout.LayoutParams siteLp = new LinearLayout.LayoutParams(dp(118), dp(48));
        siteLp.leftMargin = dp(6);
        top.addView(site, siteLp);
        site.setOnClickListener(v -> {
            try { startActivity(new Intent(Intent.ACTION_VIEW, Uri.parse(SITE_URL))); }
            catch (Exception ignored) {}
        });

        newsView = new WebView(this);
        newsView.setBackgroundColor(0xff101723);
        WebSettings ws = newsView.getSettings();
        ws.setJavaScriptEnabled(true);
        ws.setDomStorageEnabled(true);
        ws.setLoadsImagesAutomatically(true);
        ws.setUseWideViewPort(true);
        ws.setLoadWithOverviewMode(true);
        ws.setBuiltInZoomControls(false);
        ws.setDisplayZoomControls(false);
        newsView.setWebViewClient(new WebViewClient());
        root.addView(newsView, new LinearLayout.LayoutParams(-1, 0, 1));
        newsView.loadUrl(NEWS_URL);

        playerArea = new FrameLayout(this);
        playerArea.setBackgroundColor(Color.BLACK);
        root.addView(playerArea, new LinearLayout.LayoutParams(-1, 0, 1));

        playerView = new PlayerView(this);
        playerView.setUseController(false);
        playerView.setPlayer(player);
        playerArea.addView(playerView, new FrameLayout.LayoutParams(-1, -1));

        radioArt = new ImageView(this);
        radioArt.setImageResource(com.vaynah.russia1player.R.drawable.radio_gr);
        radioArt.setScaleType(ImageView.ScaleType.FIT_CENTER);
        playerArea.addView(radioArt, new FrameLayout.LayoutParams(-1, -1));

        GestureDetector gestures = new GestureDetector(this, new GestureDetector.SimpleOnGestureListener() {
            @Override public boolean onDown(MotionEvent e) { return true; }
            @Override public boolean onDoubleTap(MotionEvent e) { setFullscreen(!fullscreen); return true; }
        });
        View.OnTouchListener touch = (v, e) -> gestures.onTouchEvent(e);
        playerView.setOnTouchListener(touch);
        radioArt.setOnTouchListener(touch);

        chrome = new LinearLayout(this);
        chrome.setOrientation(LinearLayout.VERTICAL);
        chrome.setPadding(dp(10), dp(6), dp(10), dp(10));
        root.addView(chrome);

        status = new TextView(this);
        status.setTextColor(0xffd3dce9);
        status.setTextSize(14);
        status.setGravity(Gravity.CENTER);
        chrome.addView(status, new LinearLayout.LayoutParams(-1, dp(34)));

        for (int i = 0; i < NAMES.length; i++) {
            final int index = i;
            Button b = button(NAMES[i]);
            chrome.addView(b, new LinearLayout.LayoutParams(-1, dp(48)));
            b.setOnClickListener(v -> selectChannel(index));
        }

        LinearLayout row = new LinearLayout(this);
        row.setGravity(Gravity.CENTER_VERTICAL);
        chrome.addView(row);

        muteButton = button("🔊");
        row.addView(muteButton, new LinearLayout.LayoutParams(dp(64), dp(48)));
        muteButton.setOnClickListener(v -> { muted = !muted; updateVolume(); });

        SeekBar seek = new SeekBar(this);
        seek.setMax(100);
        seek.setProgress(Math.round(volume * 100));
        row.addView(seek, new LinearLayout.LayoutParams(0, dp(48), 1));
        seek.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar s, int progress, boolean user) {
                if (user) { volume = progress / 100f; muted = false; updateVolume(); }
            }
            @Override public void onStartTrackingTouch(SeekBar s) {}
            @Override public void onStopTrackingTouch(SeekBar s) {}
        });

        Button epg = button("EPG");
        row.addView(epg, new LinearLayout.LayoutParams(dp(72), dp(48)));
        epg.setOnClickListener(v -> showGuide());

        Button expand = button("⛶");
        row.addView(expand, new LinearLayout.LayoutParams(dp(64), dp(48)));
        expand.setOnClickListener(v -> setFullscreen(!fullscreen));
        updateVolume();
    }

    private void showPlayer(boolean show) {
        showingPlayer = show;
        if (newsView != null) newsView.setVisibility(show ? View.GONE : View.VISIBLE);
        if (playerArea != null) playerArea.setVisibility(show ? View.VISIBLE : View.GONE);
        if (chrome != null) chrome.setVisibility(show && !fullscreen ? View.VISIBLE : View.GONE);
        if (show) player.play(); else player.pause();
    }

    private void selectChannel(int index) {
        channel = index;
        radioArt.setVisibility(index == 3 ? View.VISIBLE : View.GONE);
        playerView.setVisibility(index == 3 ? View.GONE : View.VISIBLE);
        status.setText("Подключение к трансляции…");
        player.setMediaItem(new MediaItem.Builder().setUri(STREAMS[index])
                .setMimeType(MimeTypes.APPLICATION_M3U8).build());
        player.prepare();
        if (showingPlayer) player.play();
    }

    private void updateVolume() {
        player.setVolume(muted ? 0f : volume);
        muteButton.setText(muted || volume == 0f ? "🔇" : "🔊");
    }

    private void setFullscreen(boolean value) {
        if (value && !showingPlayer) return;
        fullscreen = value;
        chrome.setVisibility(showingPlayer && !value ? View.VISIBLE : View.GONE);
        if (Build.VERSION.SDK_INT >= 30) {
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                if (value) c.hide(WindowInsets.Type.systemBars());
                else c.show(WindowInsets.Type.systemBars());
            }
        } else {
            getWindow().getDecorView().setSystemUiVisibility(value ?
                    View.SYSTEM_UI_FLAG_FULLSCREEN | View.SYSTEM_UI_FLAG_HIDE_NAVIGATION |
                            View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY : 0);
        }
        getWindow().getDecorView().requestApplyInsets();
    }

    private void showGuide() {
        if (channel != 0) {
            new AlertDialog.Builder(this).setTitle("Программа передач")
                    .setMessage("Для этого канала проверенная программа передач недоступна.")
                    .setPositiveButton("Закрыть", null).show();
            return;
        }
        AlertDialog dialog = new AlertDialog.Builder(this).setTitle("Россия 1HD · EPG")
                .setMessage("Загрузка программы…").setPositiveButton("Закрыть", null).create();
        dialog.show();
        new Thread(() -> {
            String result;
            HttpURLConnection connection = null;
            try {
                connection = (HttpURLConnection) new URL("https://epg.iptvx.one/id/rossia1").openConnection();
                connection.setConnectTimeout(8000);
                connection.setReadTimeout(8000);
                connection.setRequestProperty("User-Agent", "Mozilla/5.0 Russia1Player/1.0");
                if (connection.getResponseCode() != 200) throw new Exception("HTTP " + connection.getResponseCode());
                try (InputStream stream = connection.getInputStream(); ByteArrayOutputStream bytes = new ByteArrayOutputStream()) {
                    byte[] buffer = new byte[8192];
                    int count;
                    while ((count = stream.read(buffer)) != -1 && bytes.size() < 1024 * 1024) bytes.write(buffer, 0, count);
                    String html = bytes.toString(StandardCharsets.UTF_8.name())
                            .replaceAll("(?is)<(script|style)[^>]*>.*?</\\1>", " ");
                    result = (Build.VERSION.SDK_INT >= 24 ?
                            Html.fromHtml(html, Html.FROM_HTML_MODE_LEGACY) : Html.fromHtml(html)).toString()
                            .replaceAll("(?m)^[ \\t]+|[ \\t]+$", "").replaceAll("\\n{3,}", "\n\n").trim();
                    if (result.length() < 30) throw new Exception("Пустой ответ");
                    if (result.length() > 12000) result = result.substring(0, 12000);
                }
            } catch (Exception e) {
                result = "Не удалось загрузить программу. Проверьте интернет или повторите позже.";
            } finally { if (connection != null) connection.disconnect(); }
            String guide = result;
            runOnUiThread(() -> { if (dialog.isShowing() && !isFinishing()) dialog.setMessage(guide); });
        }).start();
    }

    @Override public void onBackPressed() {
        if (fullscreen) {
            setFullscreen(false);
        } else if (showingPlayer) {
            showPlayer(false);
        } else if (newsView != null && newsView.canGoBack()) {
            newsView.goBack();
        } else {
            super.onBackPressed();
        }
    }

    @Override protected void onStart() { super.onStart(); if (player != null && showingPlayer) player.play(); }
    @Override protected void onStop() { super.onStop(); if (player != null) player.pause(); }
    @Override protected void onDestroy() {
        if (newsView != null) { newsView.stopLoading(); newsView.destroy(); }
        playerView.setPlayer(null);
        player.release();
        super.onDestroy();
    }
    @Override protected void onSaveInstanceState(Bundle state) {
        state.putInt("channel", channel);
        state.putFloat("volume", volume);
        state.putBoolean("muted", muted);
        state.putBoolean("fullscreen", fullscreen);
        state.putBoolean("showingPlayer", showingPlayer);
        super.onSaveInstanceState(state);
    }
}
