---
license: cc-by-4.0
language:
- en
tags:
- code
size_categories:
- 10M<n<100M
---

# HeuriGen

HeuriGen is a benchmark and agentic evaluation framework designed to rigorously assess Large Language Models (LLMs) on combinatorial optimization (CO) problems — a domain where success requires more than pattern recognition: it demands creative algorithm design, multi-step planning, tool use, and adaptive reasoning.

## 🧠 Motivation

While LLMs have shown impressive capabilities in coding and open-ended reasoning, existing benchmarks fall short:
- Objective benchmarks (e.g., HumanEval, AIME) are prone to saturation and fail to test creativity or multi-step reasoning.
- Subjective evaluations (e.g., Chatbot Arena) allow diverse outputs but often rely on noisy or superficial feedback.

To bridge this gap, HeuriGen introduces real-world CO tasks that:
- Feature well-defined objectives with expansive solution spaces.
- Require heuristic design, not just memorized answers.
- Enable quantitative and automated evaluation through code execution.

## Problem Set

| Problem | Domain |
| :--: | :--: |
| [Operator Scheduling]() | Electronic Design Automation |
| [E-Graph Extraction]() | Compilers |
| [Pickup and Delivery w/ Time Windoes]() | Logistics |
| [Technology Mapping]() | Electronic Design Automation |
| [Global Routing]() | Electronic Design Automation |
| [Protein Sequence Design]() | Computational Biology |
| [Airline Crew Pairing]() | Logistics |
| [Pedigree]() | Computational Biology |
| [Intra-Op Parallelism]() | Compilers |