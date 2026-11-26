package com.carplaybox.audio;

import android.app.Activity;
import android.graphics.Color;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.View;
import android.widget.Button;
import android.widget.EditText;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.TextView;

final class Ui {
    static final int BACKGROUND = Color.rgb(20, 24, 30);
    static final int PANEL = Color.rgb(31, 37, 45);
    static final int TEXT = Color.rgb(238, 242, 247);
    static final int MUTED = Color.rgb(165, 176, 188);
    static final int ACCENT = Color.rgb(75, 160, 255);
    static final int ERROR = Color.rgb(255, 145, 145);

    private Ui() {}

    static int dp(Activity activity, int value) {
        return Math.round(value * activity.getResources().getDisplayMetrics().density);
    }

    static LinearLayout vertical(Activity activity) {
        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.VERTICAL);
        return layout;
    }

    static LinearLayout horizontal(Activity activity) {
        LinearLayout layout = new LinearLayout(activity);
        layout.setOrientation(LinearLayout.HORIZONTAL);
        layout.setGravity(Gravity.CENTER_VERTICAL);
        return layout;
    }

    static TextView text(Activity activity, String value, float size, int color) {
        TextView view = new TextView(activity);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(color);
        view.setPadding(0, dp(activity, 2), 0, dp(activity, 2));
        return view;
    }

    static TextView title(Activity activity, String value) {
        TextView view = text(activity, value, 20, TEXT);
        view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        return view;
    }

    static TextView section(Activity activity, String value) {
        TextView view = text(activity, value, 16, ACCENT);
        view.setTypeface(Typeface.DEFAULT, Typeface.BOLD);
        view.setPadding(0, dp(activity, 12), 0, dp(activity, 4));
        return view;
    }

    static Button button(Activity activity, String value, View.OnClickListener listener) {
        Button button = new Button(activity);
        button.setText(value);
        button.setTextSize(14);
        button.setAllCaps(false);
        button.setMinHeight(0);
        button.setMinimumHeight(0);
        button.setPadding(dp(activity, 12), dp(activity, 5), dp(activity, 12), dp(activity, 5));
        button.setOnClickListener(listener);
        return button;
    }

    static EditText number(Activity activity, String hint) {
        EditText field = new EditText(activity);
        field.setHint(hint);
        field.setTextColor(TEXT);
        field.setHintTextColor(MUTED);
        field.setTextSize(15);
        field.setSingleLine(true);
        field.setSelectAllOnFocus(true);
        field.setInputType(android.text.InputType.TYPE_CLASS_NUMBER);
        field.setPadding(dp(activity, 8), dp(activity, 4), dp(activity, 8), dp(activity, 4));
        return field;
    }

    static LinearLayout.LayoutParams matchWrap() {
        return new LinearLayout.LayoutParams(LinearLayout.LayoutParams.MATCH_PARENT,
                LinearLayout.LayoutParams.WRAP_CONTENT);
    }

    static LinearLayout.LayoutParams weight() {
        return new LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f);
    }

    static ProgressBar progress(Activity activity) {
        ProgressBar progress = new ProgressBar(activity);
        progress.setIndeterminate(true);
        progress.setVisibility(View.GONE);
        return progress;
    }
}