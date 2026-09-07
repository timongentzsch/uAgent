# Research basis for self-improvement review

Source check: 2026-09-07. This is a targeted review of primary research and
engineering guidance, not a systematic literature review or certification.
The workflow is research-informed; the cited authors have not evaluated this
implementation. Recent arXiv reports below are preprints, not a consensus
standard. Local tests establish software behavior, not published research
results or general gains in agent capability.

## Evidence and implementation choices

| Topic | Published support | Our implementation and its limits |
| --- | --- | --- |
| Outcomes and review | Anthropic recommends outcome checks, deterministic graders where appropriate, human validation and transcript inspection. It distinguishes capability from regression evaluation. [1] | Require existing gates plus a candidate-pass/original-fail check. Present the exact patch and the author's assessment for human review. Re-executing an author-written test is not independently designing an oracle. |
| Stable environment | Anthropic recommends clean trial environments [1] and experimentally shows that resource configuration can change coding-agent scores. [2] | Use fresh source copies and HOME, freeze limits and gate mode, and run the unchanged baseline before discovery. Baseline preflight is our engineering response, not a procedure experimentally validated by either article. It detects failed verification without diagnosing its cause. Host load, caches and resource allocation remain uncontrolled. |
| Self-modifying source | Darwin Gödel Machine empirically evaluates source changes using coding benchmarks and tests transfer across models and languages. [3] | Source improvement followed by evaluation has research precedent. Our bounded sequential loop is not a DGM reproduction; it lacks its open-ended archive search and transfer experiments. We do not inherit its reported gains. |
| Generalization | SEAGym separates training, update validation, held-out transfer, replay and cost, and reports that frequent updates need not improve held-out performance. [4] | Preserve source-validation evidence independently of recursive outcomes. Reports explicitly mark generalization as unestablished. General performance claims need separately held-out tasks shared across versions and repeated trials; repository-visible scenarios do not qualify as held out. Those experiments are not automatically run here. |
| Focused verification | HarnessLens reports behavior-relevant verification with an attributable-evidence gate and separate held-out performance results. [5] | Focused regressions can justify narrow fixes; passing one does not establish broader capability. We retain all existing gates and do not implement HarnessLens task selection or reproduce its efficiency results. |
| Integrity | The harness-tampering study reports apparent gains and integrity violations arising from harness edits, including in real self-improvement trajectories. [6] | Freeze controller inputs, reject changed existing gates, retain provenance, and bind promotion approval to a review ID. These are our mitigations, not a tested defense against all tampering. Host verification and agent telemetry are not adversarially secure. |

## Claims we can make

Controller integration tests verify that baseline failure prevents discovery,
candidate checks are rerun in clean copies, review produces an applicable
patch without activation, incomplete assessments block review, and promotion
requires explicit approval tied to current evidence. They use scripted
executors with the native sandbox; they do not measure a live model's ability
to find useful changes.

A `source_validated` result means that the frozen gates passed and the declared
check passed on the candidate and failed on the original under recorded
conditions. Human review must still assess the check's meaning, missing edge
cases and whether the diff explains the observed result. The agent's impact
narrative and generality recommendation remain attributed assessments.

For broader performance claims, define the task population, independent
success criteria, held-out split, resource settings and decision rule before
evaluation. Reuse the existing eval runner for repeated trials and report
outcomes and costs separately. This protocol recommendation draws on the
evaluation concerns above; this change does not supply a held-out corpus or
establish any speed, cost, reliability or generalization gain.

## Deliberate design choices, not research findings

- The normal endpoint is human review of a focused fix. Making recursive A/B
  optional prevents an unrelated capability comparison from being the sole
  route for proposing an ordinary bug fix. This is a product decision.
- A review ID binds the patch, assessment and evidence. It cannot prove that a
  person actually read the report. Commit/PR presentation and authorization
  are operator responsibilities; the controller offers no commit/PR endpoint.
- Alternating pair order reduces a fixed ordering confound, but does not remove
  infrastructure noise. One pair and the default 10% selection thresholds are
  exploratory heuristics, not confidence intervals or research-derived cutoffs.
- Claim-before-edit remains an instruction. It is not enforced preregistration.
- Unlimited model calls are a configurable policy within remaining token,
  time, tool, run and cost bounds. No cited work establishes that our default
  budgets or removing the call cap is optimal.
- Host verification is a supervised local compatibility option. A temporary
  directory is not a security boundary. Disposable, appropriately isolated
  infrastructure is a separate operational requirement for unattended use.

## Primary sources

1. Anthropic. [Demystifying evals for AI agents](https://www.anthropic.com/engineering/demystifying-evals-for-ai-agents).
   2026-01-09. Engineering guidance; see coding-agent grading, nondeterminism,
   stable environments, grader design and transcript review.
2. Anthropic. [Quantifying infrastructure noise in agentic coding evals](https://www.anthropic.com/engineering/infrastructure-noise).
   2026-02-05. Primary engineering experiment on resource configuration;
   its effects are specific to the reported benchmark and setup.
3. Sakana AI. [The Darwin Gödel Machine: AI that improves itself by rewriting its own code](https://sakana.ai/dgm/).
   2025-05-30. Authors' research report; empirical self-modification,
   archive-based exploration, and transfer evaluation.
4. Zheng et al. [SEAGym: An Evaluation Environment for Self-Evolving LLM Agents](https://arxiv.org/abs/2606.17546v1).
   2026-06-16, arXiv v1. Claims used here are stated in the abstract.
5. Xu et al. [Verify Smarter, Evolve Further: Efficient Harness Evolution through Behavior-Aware Verification](https://arxiv.org/abs/2608.27311v1).
   2026-08-27, arXiv v1. Claims used here are stated in the abstract; we have
   not reproduced the reported HarnessLens results.
6. Wang et al. [Auditing Harness Tampering in Self-Improving Agents](https://arxiv.org/abs/2609.00069v1).
   2026-08-30, arXiv v1. Claims used here are stated in the abstract; this
   research identifies a risk, not a certification of our mitigations.
