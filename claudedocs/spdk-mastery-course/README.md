# SPDK Mastery Course

**Complete training program for becoming an SPDK expert and contributor**

---

## Quick Start

**New to the course?** Start here:

1. Read [00-MASTER-GUIDELINE.md](./00-MASTER-GUIDELINE.md) - Course structure and standards
2. Review [01-COURSE-INDEX.md](./01-COURSE-INDEX.md) - Complete module listing
3. Begin with [Module 01](./stage-1-foundation/01-S1-Why-SPDK-Exists.md) - Why SPDK Exists

---

## Course Philosophy

This course uses a **top-down approach**: Architecture first, then implementation, finally code details.

**Learning Path**: Understand WHY → Learn WHAT → Master HOW

---

## Three-Stage Structure

### 📚 Stage 1: Foundation (3 weeks)
**Focus**: Architecture, principles, and design philosophy

Master the conceptual foundation of SPDK without getting lost in code details. Understand the "why" behind every design decision.

**Modules**: 7 modules (~17 hours)
**Start**: [stage-1-foundation/](./stage-1-foundation/)

---

### 💻 Stage 2: Implementation (5 weeks)
**Focus**: APIs, coding patterns, and application development

Learn to build SPDK applications and modules. Hands-on coding with real examples from the codebase.

**Modules**: 10 modules (~35 hours)
**Start**: [stage-2-implementation/](./stage-2-implementation/)

---

### 🎯 Stage 3: Mastery (8 weeks)
**Focus**: Advanced topics, optimization, and contribution

Deep dive into complex subsystems, performance tuning, and becoming an SPDK contributor.

**Modules**: 13 modules (~50 hours)
**Start**: [stage-3-mastery/](./stage-3-mastery/)

---

## Current Status

**Overall Progress**: 3% (1 of 32 core modules complete)

| Stage | Modules | Status |
|-------|---------|--------|
| Stage 1 | 1/7 | 🔄 Module 01 complete |
| Stage 2 | 0/10 | 📝 Not started |
| Stage 3 | 0/13 | 📝 Not started |

---

## Course Features

### ✅ Comprehensive Coverage
- All major SPDK components
- From basics to advanced topics
- Real codebase examples

### ✅ Structured Learning
- Clear prerequisites
- Progressive complexity
- Measurable objectives

### ✅ Hands-On Practice
- 10+ practical exercises
- Real-world scenarios
- Code walkthroughs

### ✅ Reference Materials
- API quick reference
- Common patterns
- Debugging guides
- Performance tuning

---

## Target Audience

**Primary**: New employees at storage service companies

**Prerequisites**:
- Go and/or C++ experience
- Linux command line proficiency
- Programming fundamentals

**Goals**:
- Contribute to SPDK development
- Become SPDK expert
- Build production storage systems

---

## Course Structure

```
claudedocs/spdk-mastery-course/
│
├── 00-MASTER-GUIDELINE.md    ← Start here (course standards)
├── 01-COURSE-INDEX.md        ← Complete module listing
├── README.md                 ← This file
│
├── stage-1-foundation/       ← Architecture & principles
│   ├── 01-S1-Why-SPDK-Exists.md        ✅ Complete
│   ├── 02-S1-Core-Principles.md        📝 Planned
│   └── ...
│
├── stage-2-implementation/   ← APIs & coding
│   ├── 10-S2-Environment-Setup.md      📝 Planned
│   ├── 11-S2-Hello-World.md            📝 Planned
│   └── ...
│
├── stage-3-mastery/          ← Advanced topics
│   ├── 20-S3-NVMf-Architecture.md      📝 Planned
│   ├── 21-S3-Vhost-Internals.md        📝 Planned
│   └── ...
│
├── exercises/                ← Hands-on labs
│   ├── EX01-Hello-Bdev.md              📝 Planned
│   └── ...
│
├── reference/                ← Quick references
│   ├── REF-API-Quick-Reference.md      📝 Planned
│   ├── REF-Common-Patterns.md          📝 Planned
│   └── ...
│
└── assets/                   ← Diagrams & code
    ├── diagrams/
    └── code-snippets/
```

---

## How to Use This Course

### For Self-Study

1. **Sequential Learning**: Follow modules in order
2. **Time Investment**: ~15 hours/week for 16 weeks
3. **Practice**: Complete all exercises
4. **Review**: Use reference materials as needed

### For Teams

1. **Onboarding**: Use as structured training program
2. **Discussion**: Review modules as a group
3. **Mentorship**: Senior engineers guide through advanced topics
4. **Projects**: Apply learning to real company projects

### For Instructors

1. **Guideline**: Follow [00-MASTER-GUIDELINE.md](./00-MASTER-GUIDELINE.md)
2. **Consistency**: Maintain standards across modules
3. **Updates**: Keep aligned with SPDK releases
4. **Feedback**: Track student questions and improve content

---

## Getting Started

### Immediate Next Steps

1. ✅ Review the [Master Guideline](./00-MASTER-GUIDELINE.md)
2. ✅ Check [Course Index](./01-COURSE-INDEX.md) for overview
3. 🔄 **Start learning**: [Module 01 - Why SPDK Exists](./stage-1-foundation/01-S1-Why-SPDK-Exists.md)
4. 📝 Prepare your environment (covered in Stage 2, Module 10)

### Building the Rest of the Course

This is a **long-term project** that will be developed systematically:

**Development Strategy**:
- Follow the guideline strictly for consistency
- Complete Stage 1 before moving to Stage 2
- Test all code examples against current SPDK version
- Update index as modules are completed

**Estimated Timeline**:
- Stage 1 completion: 2 weeks
- Stage 2 completion: 3 weeks
- Stage 3 completion: 4 weeks
- Exercises & reference: 2 weeks
- **Total**: ~11 weeks development time

---

## Contributing to the Course

### For Course Developers

If you're helping build this course:

1. Read [00-MASTER-GUIDELINE.md](./00-MASTER-GUIDELINE.md) completely
2. Choose a module from [01-COURSE-INDEX.md](./01-COURSE-INDEX.md)
3. Follow the template structure exactly
4. Test all code examples
5. Update the index with your progress

### Quality Standards

Every module must:
- Follow the guideline template
- Include clear learning objectives
- Provide tested code examples
- Reference actual SPDK source
- Include hands-on elements
- Pass quality checklist

---

## SPDK Version Tracking

**Target SPDK Version**: Latest stable release

**Verification**:
```bash
cd /path/to/spdk
git describe --tags
```

**Update Policy**:
- Review course quarterly
- Update for major SPDK releases
- Verify code examples remain valid
- Update file paths and line numbers

---

## Support and Resources

### Official SPDK Resources

- **Website**: https://spdk.io
- **Documentation**: https://spdk.io/doc/
- **GitHub**: https://github.com/spdk/spdk
- **Mailing List**: https://lists.linuxfoundation.org/mailman/listinfo/spdk

### Course Resources

- **Course Issues**: Track in your organization's system
- **Questions**: Discuss with mentors/instructors
- **Improvements**: Suggest via feedback channels

---

## Module Status Summary

### Stage 1: Foundation
- ✅ Module 01: Why SPDK Exists
- 📝 Module 02: Core Principles
- 📝 Module 03: Architecture Overview
- 📝 Module 04: Threading Model
- 📝 Module 05: Memory Management
- 📝 Module 06: Build System
- 📝 Module 07: Codebase Navigation

### Stage 2: Implementation
- 📝 All modules planned, not started

### Stage 3: Mastery
- 📝 All modules planned, not started

**Legend**: ✅ Complete | 🔄 In Progress | ✏️ Draft | 📝 Planned

---

## Revision History

| Date | Version | Changes |
|------|---------|---------|
| 2026-01-30 | 1.0 | Initial course structure and Module 01 complete |

---

## Next Steps for Course Development

### Immediate Priorities (Week 1-2)

1. Complete remaining Stage 1 modules (02-07)
2. Create first set of exercises
3. Build basic reference materials

### Short-term Goals (Week 3-5)

1. Complete Stage 2 modules (10-19)
2. Create Stage 2 exercises
3. Test all code examples

### Medium-term Goals (Week 6-10)

1. Complete Stage 3 modules (20-32)
2. Create advanced exercises
3. Build comprehensive reference

### Long-term Maintenance

1. Quarterly SPDK version checks
2. Integrate learner feedback
3. Add new modules for emerging features
4. Keep examples current

---

## Contact

**Course Maintainer**: [Your Organization]
**Last Updated**: 2026-01-30

---

**Ready to begin?** Start with [Module 01: Why SPDK Exists](./stage-1-foundation/01-S1-Why-SPDK-Exists.md)

---

*Building high-performance storage systems, one module at a time.*
