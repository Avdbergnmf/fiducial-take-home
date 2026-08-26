# Design

<!--
Skeleton only. Every line below is a prompt to myself, not content. Delete the
prompts as each section gets written. Target 2-5 pages; this document is graded
as heavily as the code, so it gets real time, not the last hour.
-->

## Overview

- What does the brain do, in five sentences, for someone who has not read the code?
- What is the single design decision this whole document is defending?

## Module structure and why

- What are the boundaries between flight, protocol, policy and belief, and what
  exactly crosses each one?
- Which module owns time, and which ones are pure functions of their inputs?
- Does this split survive the tier-5 insider problem, or does the insider case
  force policy and belief to merge? Say which, and why.
- What did I try first and throw away?

## Wire protocol

- What is on the wire, field by field, and what is deliberately not on it?
- How is a version negotiated, and what happens when two versions meet?
- What does a receiver do with a message it does not understand — drop, relay, or
  quarantine?

## How I decide what an aircraft is

- What evidence goes into the classification, and how is it weighted?
- What is the cost of a false positive versus a false negative here, and does the
  decision rule reflect that asymmetry?
- How long does a classification persist, and what revises it?

## How I treat a peer I cannot verify

- What is the default posture towards an unverified peer: trust, ignore, or use
  with discount?
- What can an unverified peer still usefully contribute?
- Where is the line between "not yet verified" and "failed verification"?

## What I do about a compromised member

- How is a member named as compromised, and by whom?
- What happens after naming? Note: continuing to relay its traffic after naming
  it is not a response — say what actually changes.
- Is the action reversible, and what would reverse it?
- What stops a healthy member from being named by a malicious one?

## Bandwidth policy

- What is the budget, and what is sent when the budget is tight?
- What is dropped first, and who decides?
- How does the policy degrade — gracefully, or off a cliff?

## Testing approach

- What is tested deterministically, and what is only tested by running scenarios?
- What does the fixture trace prove?
- How do I know a change is an improvement and not noise?

## What I would do with another week

- The three things, in priority order, with the reason each is not done.

## Known gaps and what I did not get to

- Be specific and honest. A named gap costs less than a discovered one.
