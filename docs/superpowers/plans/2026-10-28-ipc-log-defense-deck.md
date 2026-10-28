# IPC 日志系统答辩 HTML Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce one editable, evidence-grounded, 15-slide Chinese HTML defense deck.

**Architecture:** Reuse the supplied HTML template's 16:9 layout and navigation, replacing all unrelated slide content with project-specific HTML/CSS slides. Keep the deck self-contained and avoid overwriting the root template or generator.

**Tech Stack:** Python standard library to compose the HTML template, CSS, and existing navigation script.

**Spec:** `docs/superpowers/specs/2026-10-28-ipc-log-defense-deck.md`

## Global Constraints

- Linux, C++11, CMake, FDBus 5.4.0, and Protobuf are the project requirements.
- AT1–AT5 definitions are absent from supplied materials; do not invent them.
- Do not claim current Ubuntu build or runtime acceptance; it remains pending.
- Do not overwrite the root HTML template or modify its original generator script.
- Mark visual browser validation as pending if local-file browser access is unavailable.

## Review Focus

- Unsupported assertions about passing demos or current Linux validation: mark pending.
- Misrepresenting ACK as output/persistence confirmation: explain queue-acceptance only.
- Implying strict global ordering or crash-safe/fsync durability: state finite-window and normal-exit bounds.
- Attributing an unverified historical defect to AI: present the correction as human-led requirements review.
- Unreadable diagrams or dense slide copy: check every rendered slide at full size.

---

### Task 1: Build the editable presentation

**Files:**
- Read: `校招生实习题目：IPC日志系统设计与实现.md`, `项目架构.md`, `docs/implementation-plan.md`, `docs/code-review-2026-10-27.md`, `沟通问题与AI纠偏记录.md`, current source anchors.
- Create: `答辩材料/IPC日志系统答辩-2026-10-28.html`
- Create: `scripts/generate_defense_html.py`

- [x] Create the 15-slide HTML deck using the order and claims in the spec.
- [x] Build architecture, flow, and comparison diagrams with HTML/CSS and add code anchors.
- [x] Keep the supplied root HTML template and original generator script unchanged.
- [ ] Browser visual review is unavailable because the browser URL policy blocks local `file://` navigation; do not report it as passed.

**Expected:** 15 slides in a single 16:9 HTML file, project-specific content only, editable text and controls, no unsupported validation claims.

### Task 2: Validate and deliver

**Files:**
- Final: `答辩材料/IPC日志系统答辩-2026-10-28.html`

- [x] Check the HTML slide count, navigation hooks, output path, and assignment coverage.
- [ ] Complete a browser visual review if a permitted local preview route is available.
- [x] Keep Ubuntu runtime validation pending and AT1–AT5 undefined in the materials.

**Expected:** A locally generated HTML deck is delivered; browser preview and runtime validation limitations remain visible in the project status.
