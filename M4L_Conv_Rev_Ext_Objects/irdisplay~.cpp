
#include <ext.h>
#include <ext_obex.h>
#include <z_dsp.h>

#include <HIRT_Memory.hpp>
#include <HIRT_Buffer_Access.hpp>

#include <algorithm>


t_class *this_class;


struct t_irdisplay
{
    t_pxobject x_obj;

    // Attributes

    t_atom_long read_chan;
    t_atom_long write_chan;
    t_atom_long resize;

    // Bang Outlet

    void *process_done;
};


void *irdisplay_new();
void irdisplay_free(t_irdisplay *x);
void irdisplay_assist(t_irdisplay *x, void *b, long m, long a, char *s);


void irdisplay_process(t_irdisplay *x, t_symbol *sym, long argc, t_atom *argv);
void irdisplay_process_internal(t_irdisplay *x, t_symbol *sym, short argc, t_atom *argv);


double pow_table[8194];


int C74_EXPORT main()
{
    long i;

    this_class = class_new("irdisplay~",
                           (method) irdisplay_new,
                           (method)irdisplay_free,
                           sizeof(t_irdisplay),
                           0L,
                           0);

    class_addmethod(this_class, (method)irdisplay_process, "process", A_GIMME, 0L);
    class_addmethod(this_class, (method)irdisplay_assist, "assist", A_CANT, 0L);

    CLASS_STICKY_ATTR(this_class, "category", 0, "Buffer");

    CLASS_ATTR_ATOM_LONG(this_class, "writechan", 0, t_irdisplay, write_chan);
    CLASS_ATTR_FILTER_MIN(this_class, "writechan", 1);
    CLASS_ATTR_LABEL(this_class,"writechan", 0, "Buffer Write Channel");

    CLASS_ATTR_ATOM_LONG(this_class, "resize", 0, t_irdisplay, resize);
    CLASS_ATTR_STYLE_LABEL(this_class,"resize", 0, "onoff","Buffer Resize");

    CLASS_ATTR_ATOM_LONG(this_class, "readchan", 0, t_irdisplay, read_chan);
    CLASS_ATTR_FILTER_MIN(this_class, "readchan", 1);
    CLASS_ATTR_LABEL(this_class,"readchan", 0, "Buffer Read Channel");

    CLASS_STICKY_ATTR_CLEAR(this_class, "category");

    class_register(CLASS_BOX, this_class);

    // Set up static table for fast power approximation

    for (i = 0; i < 8194; i++)
        pow_table[i] = copysign(pow(fabs((i - 4096) / 4096.0), 0.4), i - 4096.0);

    return 0;
}


void *irdisplay_new()
{
    t_irdisplay *x = (t_irdisplay *)object_alloc (this_class);

    x->process_done = bangout(x);

    x->read_chan = 1;
    x->write_chan = 1;
    x->resize = 1;

    return x;
}


void irdisplay_free(t_irdisplay *x)
{
}


void irdisplay_assist(t_irdisplay *x, void *b, long m, long a, char *s)
{
    if (m == ASSIST_INLET)
        sprintf(s,"Instructions In");
    else
        sprintf(s,"Bang on Success");
}


double power_scale(double val)
{
    val = val * 4096.0 + 4096.0;
    val = val < 0 ? 0 : val;
    val = val > 8193.0 ? 8193.0 : val;

    long idx = static_cast<long>(val);
    double fract = val - idx;

    double lo = pow_table[idx];
    double hi = pow_table[idx + 1];

    return lo - fract * (lo - hi);
}


// Arguments are - target buffer 1 / source buffer 1 / [target buffer 2 / source buffer 2] / [multiplier 1] / [multplier 2]

void irdisplay_process(t_irdisplay *x, t_symbol *sym, long argc, t_atom *argv)
{
    defer(x, (method) irdisplay_process_internal, sym, (short) argc, argv);
}


void irdisplay_process_internal(t_irdisplay *x, t_symbol *sym, short argc, t_atom *argv)
{
    t_symbol *target2 = nullptr;
    t_symbol *source2 = nullptr;

    double source_vol1 = 1.0;
    double source_vol2 = 1.0;

    double sample_rate1 = 0.0;
    double sample_rate2 = 0.0;
    float max = 0.f;
    float max1 = 0.f;
    float max2 = 0.f;

    t_ptr_int length1 = 0;
    t_ptr_int length2 = 0;

    // Check arguments

    if (argc < 2)
    {
        object_error((t_object *)x, "not enough arguments to message %s", sym->s_name);
        return;
    }

    t_symbol *target1 = atom_getsym(argv++);
    argc--;
    t_symbol *source1 = atom_getsym(argv++);
    argc--;

    if (argc && atom_gettype(argv) == A_SYM)
    {
        target2 = atom_getsym(argv++);
        argc--;

        if (!argc || atom_gettype(argv) != A_SYM)
        {
            object_error((t_object *)x, "no source buffer given for second target");
            return;
        }

        source2 = atom_getsym(argv++);
        argc--;
    }

    if (argc)
    {
        source_vol1 = atom_getfloat(argv++);
        argc--;
    }

    if (argc)
    {
        source_vol2 = atom_getfloat(argv++);
        argc--;
    }

    t_atom_long read_chan = x->read_chan - 1;

    // Check source buffer

    if (buffer_check((t_object *) x, source1, read_chan) || !source1)
        return;
    
    length1 = buffer_length(source1);
    sample_rate1 = buffer_sample_rate(source1);

    if (source2)
    {
        if (buffer_check((t_object *) x, source2, read_chan))
            return;
        length2 = buffer_length(source2);
        sample_rate2 = buffer_sample_rate(source2);
    }

    temp_ptr<double> temp_d(length1 + length2);
    temp_ptr<float>  temp_f(length1 + length2);

    if (!temp_d || !temp_f)
    {
        object_error((t_object *)x, "could not allocate temporary memory for processing");
        return;
    }

    double *output1 = temp_d.get();
    double *output2 = output1 + length1;
    float *temp1 = temp_f.get();
    float *temp2 = temp1 + length1;

    // Read buffers

    buffer_read(source1, read_chan, temp1, length1);
    if (source2)
        buffer_read(source2, read_chan, temp2, length2);

    // Find maximums and calculate scaling

    for (t_ptr_int i = 0; i < length1; i++)
        max1 = std::max(fabsf(temp1[i]), max1);

    if (source2)
    {
        for (t_ptr_int i = 0; i < length2; i++)
            max2 = std::max(fabsf(temp2[i]), max2);
    }

    max = static_cast<float>(max1 * source_vol1 > max2 * source_vol2 ? max1  * source_vol1 : max2 * source_vol2);
    max = 1.f / max;
    source_vol1 *= max;
    source_vol2 *= max;

    // Map to new scaling

    for (t_ptr_int i = 0; i < length1; i++)
        output1[i] = power_scale(temp1[i] * source_vol1);
    if (source2)
        for (t_ptr_int i = 0; i < length2; i++)
            output2[i] = power_scale(temp2[i] * source_vol2);

    // Write to output(s)

    auto error = buffer_write((t_object *)x, target1, output1, length1, x->write_chan - 1, static_cast<long>(x->resize), sample_rate1, 1);

    if (!error && source2)
    {
        error = buffer_write((t_object *)x, target2, output2, length2, x->write_chan - 1, static_cast<long>(x->resize), sample_rate2, 1);
    }

    if (!error)
        outlet_bang(x->process_done);
}

